;;; emacs_slime_test.el --- batch SLIME smoke test against klisp -*- lexical-binding: t; -*-
;;
;; Drives a REAL Emacs + SLIME client against a running klisp SWANK server:
;; connects, waits for the handshake, evaluates expressions, and checks error
;; recovery. Exits 0 on success, 1 on failure. Port via env KLISP_PORT (4005).
;;
;; Run: KLISP_PORT=4099 emacs --batch -l scripts/emacs_slime_test.el

(require 'cl-lib)

(let ((d (car (file-expand-wildcards
               (expand-file-name "~/.emacs.d/elpa/slime-*")))))
  (unless d (princ "FAIL: SLIME not found in ~/.emacs.d/elpa\n") (kill-emacs 2))
  (add-to-list 'load-path d))
(require 'slime)
(setq slime-protocol-version 'ignore)   ; tolerate version differences
(setq slime-contribs nil)               ; avoid slime-fancy (needs macrostep)
;; load the contribs a normal user has, minus the macrostep-dependent ones:
;; slime-repl (REPL + create-repl), slime-autodoc (eldoc-on-type), and
;; slime-presentations (clickable/inspectable REPL output).
(slime-setup '(slime-repl slime-autodoc slime-presentations))

(defvar klisp-port (string-to-number (or (getenv "KLISP_PORT") "4005")))
(defvar klisp-connected nil)
(add-hook 'slime-connected-hook (lambda () (setq klisp-connected t)))

(defvar klisp-fails 0)
(defun klisp-fail (fmt &rest args)
  (princ (apply #'format (concat "FAIL: " fmt "\n") args))
  (kill-emacs 1))
(defun klisp-check (ok name detail)
  (princ (format "[%s] %s %s\n" (if ok "ok  " "FAIL") name detail))
  (unless ok (cl-incf klisp-fails)))

(defun klisp-eval (code)
  "Evaluate CODE (a string) in klisp via SLIME, returning the result string."
  (slime-eval `(swank:interactive-eval ,code)))

(princ (format "connecting to localhost:%d ...\n" klisp-port))
(condition-case e
    (slime-connect "localhost" klisp-port)
  (error (klisp-fail "slime-connect signaled: %S" e)))

(with-timeout (30 (klisp-fail "timed out waiting for SLIME handshake"))
  (while (not klisp-connected)
    (accept-process-output nil 0.2)))
(princ (format "SLIME connected: %s\n"
               (slime-connection-name (slime-current-connection))))

;; basic evaluation through the real client
(dolist (tc '(("(+ 1 2)" . "3")
              ("(* 6 7)" . "42")
              ("(do (= sq (fn (n) (* n n))) (sq 9))" . "81")))
  (let* ((code (car tc)) (want (cdr tc))
         (got (condition-case e (klisp-eval code)
                (error (format "<error %S>" e)))))
    (klisp-check (and (stringp got) (string-match-p (regexp-quote want) got))
                 code (format "=> %S (want %S)" got want))))

;; functions + recursion across calls (state persists in the connection)
(klisp-eval "(= fact (fn (n) (if (< n 2) 1 (* n (fact (- n 1))))))")
(let ((got (klisp-eval "(fact 5)")))
  (klisp-check (and (stringp got) (string-match-p "120" got))
               "(fact 5)" (format "=> %S" got)))

;; autodoc fires on every keystroke in a real session; SLIME destructures the
;; result as (doc &optional cache-p), so a nil result errors its process filter
;; and breaks typing. Ensure it comes back as a proper list.
(let ((ad (condition-case e
              (slime-eval '(swank:autodoc (quote ("+")) :print-right-margin 80))
            (error (format "<error %S>" e)))))
  (klisp-check (consp ad) "autodoc returns a list" (format "=> %S" ad)))

;; inspector: open it on a list and confirm SLIME renders the parts (this is
;; the real client path — slime-open-inspector parsing our istate format).
(slime-eval-async '(swank:init-inspector "(list 11 22 33)") #'slime-open-inspector)
(let ((n 0))
  (while (and (< n 100) (not (get-buffer "*slime-inspector*")))
    (accept-process-output nil 0.1) (setq n (1+ n))))
(let ((b (get-buffer "*slime-inspector*")))
  (klisp-check (and b (with-current-buffer b
                        (and (string-match-p "List" (buffer-string))
                             (string-match-p "22" (buffer-string)))))
               "inspector renders a list" ""))

;; presentations: a REPL result must be a clickable presentation that opens the
;; inspector — the "click the output object" path the user wanted.
(when (get-buffer "*slime-inspector*") (kill-buffer "*slime-inspector*"))
(let ((rb (slime-repl-buffer)))
  (if (not rb)
      (klisp-check nil "repl buffer present" "")
    (with-current-buffer rb
      (goto-char (point-max))
      (insert "(list 44 55 66)")
      (slime-repl-return)
      (let ((n 0)) (while (< n 80) (accept-process-output nil 0.1) (setq n (1+ n))))
      (let* ((pos (text-property-not-all (point-min) (point-max)
                                         'slime-repl-presentation nil))
             (pres (and pos (get-text-property pos 'slime-repl-presentation))))
        (klisp-check pres "repl result is a clickable presentation"
                     (format "at %S" pos))
        (when pres
          ;; exactly what clicking a presentation does: inspect it by id
          (slime-eval-async
              `(swank:inspect-presentation ',(slime-presentation-id pres) nil)
            #'slime-open-inspector)
          (let ((n 0) (ok nil))
            (while (and (< n 150) (not ok))
              (accept-process-output nil 0.1)
              (let ((b (get-buffer "*slime-inspector*")))
                (when (and b (with-current-buffer b
                               (string-match-p "55" (buffer-string))))
                  (setq ok t)))
              (setq n (1+ n)))
            (klisp-check ok "clicking the result opens the inspector" "")))))))

;; error recovery: a bad form aborts (SLIME signals), the next form still works
(let ((aborted nil))
  (condition-case _e (klisp-eval "(/ 1 0)") (error (setq aborted t)))
  (klisp-check aborted "(/ 1 0)" "aborts as expected")
  (let ((got (klisp-eval "(+ 40 2)")))
    (klisp-check (and (stringp got) (string-match-p "42" got))
                 "recovery (+ 40 2)" (format "=> %S" got))))

(princ (format "\n%d failures\n" klisp-fails))
(kill-emacs (if (= klisp-fails 0) 0 1))

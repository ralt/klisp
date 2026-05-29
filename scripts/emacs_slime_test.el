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
(setq slime-contribs nil)               ; base SLIME only (no slime-fancy/macrostep)
(slime-setup)

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

;; error recovery: a bad form aborts (SLIME signals), the next form still works
(let ((aborted nil))
  (condition-case _e (klisp-eval "(/ 1 0)") (error (setq aborted t)))
  (klisp-check aborted "(/ 1 0)" "aborts as expected")
  (let ((got (klisp-eval "(+ 40 2)")))
    (klisp-check (and (stringp got) (string-match-p "42" got))
                 "recovery (+ 40 2)" (format "=> %S" got))))

(princ (format "\n%d failures\n" klisp-fails))
(kill-emacs (if (= klisp-fails 0) 0 1))

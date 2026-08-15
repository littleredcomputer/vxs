// test_suite.js: In-browser & Node.js Comprehensive Scheme Unit Test Matrix

const TEST_DEFINITIONS = [
  // 1. Primitive Arithmetic & Types
  { id: 'math-add', name: 'N-ary Addition', code: '(+ 1 2 3 4 5)', expectOk: true, expectResult: '15' },
  { id: 'math-sub', name: 'N-ary Subtraction', code: '(- 100 25 15)', expectOk: true, expectResult: '60' },
  { id: 'math-neg', name: 'Unary Negation', code: '(- 42)', expectOk: true, expectResult: '-42' },
  { id: 'math-mul', name: 'N-ary Multiplication', code: '(* 2 3 4 5)', expectOk: true, expectResult: '120' },
  { id: 'math-div', name: 'Float Division', code: '(/ 100 4 5)', expectOk: true, expectResult: '5.0' },
  { id: 'math-rem', name: 'Integer Remainder', code: '(remainder 17 5)', expectOk: true, expectResult: '2' },
  { id: 'math-mod', name: 'Modulo Operation', code: '(modulo -17 5)', expectOk: true, expectResult: '-2' },
  { id: 'math-abs', name: 'Absolute Value', code: '(abs -42)', expectOk: true, expectResult: '42' },
  { id: 'math-sqrt', name: 'Square Root', code: '(sqrt 16.0)', expectOk: true, expectResult: '4.0' },

  // 2. Comparisons & Predicates
  { id: 'pred-eq', name: 'Numeric Equality', code: '(= (+ 2 2) 4)', expectOk: true, expectResult: '#t' },
  { id: 'pred-lt', name: 'Less Than', code: '(< 10 20)', expectOk: true, expectResult: '#t' },
  { id: 'pred-gt', name: 'Greater Than', code: '(> 10 20)', expectOk: true, expectResult: '#f' },
  { id: 'pred-null', name: 'Null Predicate', code: '(null? \'())', expectOk: true, expectResult: '#t' },
  { id: 'pred-null-neg', name: 'Null Predicate (non-null)', code: '(null? \'(1 2))', expectOk: true, expectResult: '#f' },
  { id: 'pred-pair', name: 'Pair Predicate', code: '(pair? \'(1 . 2))', expectOk: true, expectResult: '#t' },
  { id: 'pred-not', name: 'Logical Not', code: '(not #f)', expectOk: true, expectResult: '#t' },

  // 3. List Operations
  { id: 'list-construct', name: 'List Construction', code: '(list 1 2 3 4)', expectOk: true, expectResult: '(1 2 3 4)' },
  { id: 'list-length', name: 'List Length', code: '(length \'(a b c d e))', expectOk: true, expectResult: '5' },
  { id: 'list-reverse', name: 'List Reverse', code: '(reverse \'(1 2 3 4))', expectOk: true, expectResult: '(4 3 2 1)' },
  { id: 'list-append', name: 'List Append', code: '(append \'(1 2) \'(3 4) \'(5))', expectOk: true, expectResult: '(1 2 3 4 5)' },
  { id: 'list-cadr', name: 'cadr / caddr accessors', code: '(caddr \'(10 20 30 40))', expectOk: true, expectResult: '30' },

  // 4. Higher-Order Functions & Closures
  { id: 'fn-map', name: 'Higher-Order map', code: '(map (lambda (x) (* x 2)) \'(1 2 3 4))', expectOk: true, expectResult: '(2 4 6 8)' },
  { id: 'fn-apply', name: 'Higher-Order apply', code: '(apply + \'(10 20 30 40))', expectOk: true, expectResult: '100' },
  { id: 'fn-curry', name: 'Lexical Upvalue Capture', code: '(define (make-adder x) (lambda (y) (+ x y))) ((make-adder 10) 32)', expectOk: true, expectResult: '42' },
  { id: 'fn-fact', name: 'Recursive Factorial', code: '(define (fact n) (if (<= n 1) 1 (* n (fact (- n 1))))) (fact 6)', expectOk: true, expectResult: '720' },

  // 5. Special Forms & Macros
  { id: 'form-named-let', name: 'Named Let Tail Recursion', code: '(let loop ((i 0) (sum 0)) (if (> i 10) sum (loop (+ i 1) (+ sum i))))', expectOk: true, expectResult: '55' },
  { id: 'form-let-star', name: 'Sequential let*', code: '(let* ((x 10) (y (+ x 5)) (z (* y 2))) z)', expectOk: true, expectResult: '30' },
  { id: 'form-do', name: 'Do Iteration Macro', code: '(do ((i 0 (+ i 1)) (acc 0 (+ acc i))) ((= i 5) acc))', expectOk: true, expectResult: '10' },
  { id: 'form-cond', name: 'Cond Multi-Branch', code: '(cond ((= 1 2) \'no) ((= 2 2) \'yes) (else \'other))', expectOk: true, expectResult: 'yes' },
  { id: 'form-when', name: 'When Macro', code: '(when (= (+ 2 2) 4) 999)', expectOk: true, expectResult: '999' },
  { id: 'form-and-or', name: 'Short-Circuit And/Or', code: '(and #t (= 2 2) (or #f 77))', expectOk: true, expectResult: '77' },

  // 6. First-Class Futures & Fiber Concurrency
  { id: 'fut-touch', name: 'Future & Touch', code: '(touch (future (* 6 7)))', expectOk: true, expectResult: '42' },
  { id: 'fut-multi', name: 'Concurrent Futures Sync', code: '(define f1 (future (+ 10 20))) (define f2 (future (+ 30 40))) (+ (touch f1) (touch f2))', expectOk: true, expectResult: '100' },
  { id: 'fut-coop-fiber', name: 'Cooperative Fiber Yielding', code: '(define bg 0) (future (begin (set! bg 1) (yield) (set! bg 2))) (run-fibers) bg', expectOk: true, expectResult: '2' },

  // 7. Negative & Error-Catching Tests
  { id: 'err-unbound', name: 'Catch Unbound Variable Error', code: 'nonexistent-identifier-12345', expectOk: false, errorContains: 'Unbound global variable' },
  { id: 'err-argc-mismatch', name: 'Catch Closure Arity Mismatch', code: '((lambda (x) x) 1 2 3)', expectOk: false, errorContains: 'Closure call: expected 1 args' },
  { id: 'err-non-procedure', name: 'Catch Non-Procedure Call', code: '(42 1 2)', expectOk: false, errorContains: 'Attempted to call non-procedure' },
  { id: 'err-touch-non-future', name: 'Catch Touch on Non-Future', code: '(touch "not-a-future")', expectOk: false, errorContains: 'touch: expected a future' },
  { id: 'err-car-non-pair', name: 'Catch car Contract Violation', code: '(car 42)', expectOk: false, errorContains: 'car: contract violation, expected pair' },
  { id: 'err-cdr-non-pair', name: 'Catch cdr Contract Violation', code: '(cdr \'())', expectOk: false, errorContains: 'cdr: contract violation, expected pair' },
  { id: 'err-set-car-non-pair', name: 'Catch set-car! Contract Violation', code: '(set-car! 42 10)', expectOk: false, errorContains: 'set-car!: contract violation, expected pair' }
];

async function runTestSuite(evalJsonFn, clearFibersFn) {
  const results = [];
  let passedCount = 0;

  for (const test of TEST_DEFINITIONS) {
    if (clearFibersFn) clearFibersFn();
    const startTime = performance.now();
    let resObj;

    try {
      const rawJson = evalJsonFn(test.code);
      resObj = JSON.parse(rawJson);
    } catch (e) {
      resObj = { ok: false, error: e.message, error_type: 'parse_or_js_error' };
    }

    const elapsedMs = (performance.now() - startTime).toFixed(2);
    let passed = false;
    let failureReason = '';

    if (test.expectOk) {
      if (!resObj.ok) {
        failureReason = `Expected success, got error: ${resObj.error}`;
      } else if (test.expectResult !== undefined && resObj.result !== test.expectResult) {
        failureReason = `Expected result "${test.expectResult}", got "${resObj.result}"`;
      } else {
        passed = true;
      }
    } else {
      // Expecting failure / error
      if (resObj.ok) {
        failureReason = `Expected error, but evaluated successfully to "${resObj.result}"`;
      } else if (test.errorContains && !resObj.error.includes(test.errorContains)) {
        failureReason = `Expected error containing "${test.errorContains}", got "${resObj.error}"`;
      } else {
        passed = true;
      }
    }

    if (passed) passedCount++;

    results.push({
      id: test.id,
      name: test.name,
      code: test.code,
      passed,
      failureReason,
      resObj,
      elapsedMs
    });
  }

  return {
    total: TEST_DEFINITIONS.length,
    passed: passedCount,
    failed: TEST_DEFINITIONS.length - passedCount,
    results
  };
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = { TEST_DEFINITIONS, runTestSuite };
}

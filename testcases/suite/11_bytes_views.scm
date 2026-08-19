;;----------------------------------------------------------------------
;; Layer 11: byte buffers and typed views
;;
;; Storage is plain bytes; the element type lives on the VIEW. That is
;; both what JS does (ArrayBuffer owns memory, Float32Array is a view) and
;; what WebGPU speaks (getMappedRange hands back an ArrayBuffer), but the
;; real reason is that GPU data is not uniformly typed: a particle is
;; vec3<f32> + f32 + u32 in one buffer with padding. Typed-arrays-as-
;; storage would force one element type per buffer and turn structured
;; data into several parallel buffers that must stay index-aligned.
;;
;; Residency is a state machine. Only the host-side states exist yet —
;; building (growable) and sealed (fixed, viewable). Device and mapped
;; arrive with the WebGPU binding.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")

(test-suite "11_bytes_views: byte buffers, typed views, residency")

;; --- storage ------------------------------------------------------------

(define b (make-bytes 16))
(assert-equal "make-bytes is a byte buffer"  #t (bytes? b))
(assert-equal "make-bytes has the length asked for" 16 (bytes-length b))
(assert-equal "make-bytes arrives sealed"    'sealed (bytes-residency b))
(assert-equal "a vector is not a byte buffer" #f (bytes? (vector 1 2 3)))

(define sink (open-byte-sink))
(assert-equal "a sink starts empty"    0 (bytes-length sink))
(assert-equal "a sink starts building" 'building (bytes-residency sink))

;; --- appending, and the invariant that stops it -------------------------

(bytes-append! sink "abc")
(assert-equal "appending a string"     3 (bytes-length sink))
(bytes-append! sink 100 101)
(assert-equal "appending raw bytes"    5 (bytes-length sink))
(assert-equal "bytes->string reads it back" "abcde" (bytes->string sink))

(bytes-append! sink "fg" 104)
(assert-equal "appending several at once" "abcdefgh" (bytes->string sink))

;; Sealing is the point of no return: a sealed buffer's address is what a
;; view — and later a GPU bind group — depends on, so growing it would
;; reallocate underneath them. That must be an error, not corruption.
(bytes-seal! sink)
(assert-equal "sealing changes residency" 'sealed (bytes-residency sink))
(assert-equal "appending to a sealed buffer raises"
              #t
              (guard (e (#t #t)) (bytes-append! sink "more") #f))
(assert-equal "the sealed contents are untouched" "abcdefgh" (bytes->string sink))

;; --- typed views --------------------------------------------------------

(define fb (make-bytes 16))
(define fv (bytes-view fb 'f32))
(assert-equal "a view is a view"        #t   (view? fv))
(assert-equal "16 bytes holds 4 floats" 4    (view-length fv))
(assert-equal "the view knows its type" 'f32 (view-type fv))
(assert-equal "the view remembers its buffer" #t (eq? fb (view-bytes fv)))

(view-set! fv 0 1.5)
(view-set! fv 3 -2.25)
(assert-equal "f32 round-trips"          1.5   (view-ref fv 0))
(assert-equal "f32 round-trips negative" -2.25 (view-ref fv 3))
(assert-equal "untouched slots read zero" 0.0  (view-ref fv 1))

(define ub (make-bytes 8))
(define uv (bytes-view ub 'u32))
(view-set! uv 0 4294967295)
(view-set! uv 1 42)
(assert-equal "u32 holds its full range" 4294967295 (view-ref uv 0))
(assert-equal "u32 round-trips"          42         (view-ref uv 1))

(define iv (bytes-view (make-bytes 8) 'i32))
(view-set! iv 0 -7)
(assert-equal "i32 keeps its sign" -7 (view-ref iv 0))

(define bv (bytes-view (make-bytes 4) 'u8))
(assert-equal "u8 view sees every byte" 4 (view-length bv))
(view-set! bv 2 200)
(assert-equal "u8 round-trips" 200 (view-ref bv 2))

;; --- two views over one buffer: the array-of-structs case ---------------
;; This is the reason the type is on the view. One buffer, 3 records of
;; 16 bytes: a vec3-ish f32 triple at offset 0, and a u32 id at offset 12.

(define rec (make-bytes 48))
(define xs  (bytes-view rec 'f32 0  16))
(define ids (bytes-view rec 'u32 12 16))
(assert-equal "strided view counts records, not bytes" 3 (view-length xs))
(assert-equal "the second view counts the same records" 3 (view-length ids))

(view-set! xs 0 10.5) (view-set! ids 0 111)
(view-set! xs 1 20.5) (view-set! ids 1 222)
(view-set! xs 2 30.5) (view-set! ids 2 333)

(assert-equal "strided f32 reads back"  '(10.5 20.5 30.5)
              (list (view-ref xs 0) (view-ref xs 1) (view-ref xs 2)))
(assert-equal "interleaved u32 reads back" '(111 222 333)
              (list (view-ref ids 0) (view-ref ids 1) (view-ref ids 2)))

;; Overlaying the SAME bytes with a different type is legal and is how
;; you reinterpret: the u8 view sees the f32's raw bytes.
(define raw (bytes-view rec 'u8))
(assert-equal "a u8 view spans the whole buffer" 48 (view-length raw))
(assert-equal "writing through one view is visible through another"
              #t
              (begin (view-set! xs 0 0.0)
                     (= 0 (+ (view-ref raw 0) (view-ref raw 1)
                             (view-ref raw 2) (view-ref raw 3)))))

;; --- bounds are checked -------------------------------------------------

(assert-equal "view-ref past the end raises"  #t
              (guard (e (#t #t)) (view-ref fv 4) #f))
(assert-equal "view-ref at a negative index raises" #t
              (guard (e (#t #t)) (view-ref fv -1) #f))
(assert-equal "view-set! past the end raises" #t
              (guard (e (#t #t)) (view-set! fv 99 1.0) #f))
(assert-equal "a bad element type raises"     #t
              (guard (e (#t #t)) (bytes-view fb 'float128) #f))
(assert-equal "viewing a non-buffer raises"   #t
              (guard (e (#t #t)) (bytes-view "not bytes" 'f32) #f))

;; A view over a buffer too small for even one element is empty, not an
;; error — an emitter may legitimately produce nothing.
(assert-equal "a view needing more bytes than exist is empty"
              0 (view-length (bytes-view (make-bytes 2) 'f32)))

(suite-summary)

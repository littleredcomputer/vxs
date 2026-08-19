;;----------------------------------------------------------------------
;; The Shadertoy harness: a pure kernel wrapped in a fullscreen draw
;;
;; lib/wgsl.scm compiles a kernel and knows nothing about where it runs.
;; This file is one HARNESS for such a kernel — the smallest one: a
;; fragment shader over a fullscreen triangle, with no buffers, no compute
;; pass and no state between frames. A wrangle over a point buffer will be
;; a sibling of this file, wrapping the same compiler in strictly more
;; machinery; keeping the two apart is what stops that machinery from
;; leaking into the kernel language.
;;
;; The kernel is an expression of uv, time and res, returning vec3f colour:
;;
;;   uv    vec2f, (0,0) top-left to (1,1) bottom-right
;;   time  f32, seconds
;;   res   vec2f, canvas size in pixels
;;
;; One fullscreen TRIANGLE rather than two triangles making a quad: it
;; needs no vertex buffer at all (positions come from vertex_index), and it
;; has no seam down the diagonal where the two halves meet.
;;
;; Identifier choices worth keeping: the varying struct is passed as `v`
;; and the local as `o`, avoiding `in` and `out`, which are reserved words
;; in WGSL and would fail to compile for reasons having nothing to do with
;; the kernel.
;;----------------------------------------------------------------------

(load "lib/wgsl.scm")

;; What a Shadertoy-shaped kernel may refer to.
(define shadertoy-env '((uv . vec2f) (time . f32) (res . vec2f)))

;; The uniform block. Padded to 16 bytes because WGSL uniform buffers
;; require it; the host writes the same four floats in this order.
(define shadertoy-preamble
  (string-append
   "struct U {\n"
   "  time : f32,\n"
   "  width : f32,\n"
   "  height : f32,\n"
   "  pad : f32,\n"
   "};\n"
   "@group(0) @binding(0) var<uniform> u : U;\n"
   "\n"
   "struct VSOut {\n"
   "  @builtin(position) pos : vec4<f32>,\n"
   "  @location(0) uv : vec2<f32>,\n"
   "};\n"
   "\n"
   "@vertex\n"
   "fn vs(@builtin(vertex_index) vi : u32) -> VSOut {\n"
   "  var corners = array<vec2<f32>, 3>(\n"
   "    vec2<f32>(-1.0, -3.0),\n"
   "    vec2<f32>(-1.0,  1.0),\n"
   "    vec2<f32>( 3.0,  1.0)\n"
   "  );\n"
   "  let p = corners[vi];\n"
   "  var o : VSOut;\n"
   "  o.pos = vec4<f32>(p, 0.0, 1.0);\n"
   "  o.uv = vec2<f32>((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);\n"
   "  return o;\n"
   "}\n"
   "\n"))

(define shadertoy-epilogue
  (string-append
   "\n"
   "@fragment\n"
   "fn fs(v : VSOut) -> @location(0) vec4<f32> {\n"
   "  return vec4<f32>(kernel(v.uv, u.time, vec2<f32>(u.width, u.height)), 1.0);\n"
   "}\n"))

;; (shadertoy body) -> complete WGSL source for gpu-run-kernel!
;; `body` is a lib/wgsl.scm expression returning vec3f.
(define (shadertoy body)
  (let ((compiled (wgsl-compile body shadertoy-env)))
    (if (not (eq? (wgsl-type-of compiled) 'vec3f))
        (error 'shadertoy
               (string-append "kernel must return vec3<f32>, got: "
                              (wgsl-type-name (wgsl-type-of compiled)))))
    (string-append
     shadertoy-preamble
     "fn kernel(uv : vec2<f32>, time : f32, res : vec2<f32>) -> vec3<f32> {\n"
     (wgsl-body body shadertoy-env "  ") "\n"
     "}\n"
     shadertoy-epilogue)))

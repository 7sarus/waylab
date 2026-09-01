// SPDX-License-Identifier: GPL-2.0-only

#include "gl-effects.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>
#include "common/macros.h"
#include "view.h"

#define MAX_BLUR_PASSES 6

struct shader_prog {
	GLuint program;
	GLint a_position;
	GLint a_texcoord;
	GLint u_proj;
	GLint u_tex;
	GLint u_size;
	GLint u_radius;
	GLint u_alpha;
};

struct blur_down_prog {
	GLuint program;
	GLint a_position;
	GLint a_texcoord;
	GLint u_proj;
	GLint u_tex;
	GLint u_halfpixel;
	GLint u_offset;
};

struct blur_up_prog {
	GLuint program;
	GLint a_position;
	GLint a_texcoord;
	GLint u_proj;
	GLint u_tex;
	GLint u_halfpixel;
	GLint u_offset;
};

struct blur_composite_prog {
	GLuint program;
	GLint a_position;
	GLint a_texcoord;
	GLint u_proj;
	GLint u_tex_blur;
	GLint u_tex_bg;
	GLint u_size;
	GLint u_radius;
	GLint u_has_blur;
};

struct clip_prog {
	GLuint program;
	GLint a_position;
	GLint u_proj;
	GLint u_box_pos;
	GLint u_box_size;
	GLint u_radius;
};

struct blur_fbo {
	GLuint fbo;
	GLuint texture;
	int width;
	int height;
};

static struct {
	bool initialized;
	bool has_external_oes;
	struct wlr_egl *egl;
	struct shader_prog prog_2d;
	struct shader_prog prog_external;
	struct clip_prog prog_clip;
	struct blur_down_prog prog_blur_down;
	struct blur_up_prog prog_blur_up;
	struct blur_composite_prog prog_blur_composite;
	struct blur_fbo fbo_chain[MAX_BLUR_PASSES + 1];
	struct blur_fbo fbo_bg;
	GLuint quad_vbo;
} gl_ctx;

static const char *clip_vert_src =
	"attribute vec2 position;\n"
	"uniform mat3 proj;\n"
	"varying vec2 v_pos;\n"
	"void main() {\n"
	"    v_pos = position;\n"
	"    vec3 p = proj * vec3(position, 1.0);\n"
	"    gl_Position = vec4(p.xy, 0.0, 1.0);\n"
	"}\n";

static const char *clip_frag_src =
	"precision mediump float;\n"
	"varying vec2 v_pos;\n"
	"uniform vec2 box_pos;\n"
	"uniform vec2 box_size;\n"
	"uniform float radius;\n"
	"\n"
	"float rounded_box_sdf(vec2 p, vec2 half_size, float r) {\n"
	"    vec2 d = abs(p) - half_size + vec2(r);\n"
	"    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;\n"
	"}\n"
	"\n"
	"void main() {\n"
	"    vec2 half_size = box_size * 0.5;\n"
	"    vec2 p = v_pos - (box_pos + half_size);\n"
	"    float dist = rounded_box_sdf(p, half_size, radius);\n"
	"    if (dist <= -0.5) {\n"
	"        discard;\n"
	"    }\n"
	"    float alpha = clamp(dist + 0.5, 0.0, 1.0);\n"
	"    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0 - alpha);\n"
	"}\n";

/* Dual-Kawase Blur Downsample Shader */
static const char *blur_down_frag_src =
	"precision mediump float;\n"
	"varying vec2 v_texcoord;\n"
	"uniform sampler2D tex;\n"
	"uniform vec2 halfpixel;\n"
	"uniform float offset;\n"
	"\n"
	"void main() {\n"
	"    vec2 uv = v_texcoord;\n"
	"    vec2 d = halfpixel * offset;\n"
	"    vec4 sum = texture2D(tex, uv) * 4.0;\n"
	"    sum += texture2D(tex, uv - d);\n"
	"    sum += texture2D(tex, uv + d);\n"
	"    sum += texture2D(tex, uv + vec2(d.x, -d.y));\n"
	"    sum += texture2D(tex, uv - vec2(d.x, -d.y));\n"
	"    gl_FragColor = sum * 0.125;\n"
	"}\n";

/* Dual-Kawase Blur Upsample Shader */
static const char *blur_up_frag_src =
	"precision mediump float;\n"
	"varying vec2 v_texcoord;\n"
	"uniform sampler2D tex;\n"
	"uniform vec2 halfpixel;\n"
	"uniform float offset;\n"
	"\n"
	"void main() {\n"
	"    vec2 uv = v_texcoord;\n"
	"    vec2 d = halfpixel * offset;\n"
	"    vec4 sum = vec4(0.0);\n"
	"    sum += texture2D(tex, uv + vec2(-d.x * 2.0, 0.0));\n"
	"    sum += texture2D(tex, uv + vec2(-d.x, d.y)) * 2.0;\n"
	"    sum += texture2D(tex, uv + vec2(0.0, d.y * 2.0));\n"
	"    sum += texture2D(tex, uv + vec2(d.x, d.y)) * 2.0;\n"
	"    sum += texture2D(tex, uv + vec2(d.x * 2.0, 0.0));\n"
	"    sum += texture2D(tex, uv + vec2(d.x, -d.y)) * 2.0;\n"
	"    sum += texture2D(tex, uv + vec2(0.0, -d.y * 2.0));\n"
	"    sum += texture2D(tex, uv + vec2(-d.x, -d.y)) * 2.0;\n"
	"    gl_FragColor = sum * (1.0 / 12.0);\n"
	"}\n";

/* Dual-Kawase Blur Composite Shader (with Background Corner Restoration & Smooth Antialiasing) */
static const char *blur_composite_frag_src =
	"precision mediump float;\n"
	"varying vec2 v_texcoord;\n"
	"varying vec2 v_pos;\n"
	"uniform sampler2D tex_blur;\n"
	"uniform sampler2D tex_bg;\n"
	"uniform vec2 size;\n"
	"uniform float radius;\n"
	"uniform float has_blur;\n"
	"\n"
	"float rounded_box_sdf(vec2 p, vec2 half_size, float r) {\n"
	"    vec2 d = abs(p) - half_size + vec2(r);\n"
	"    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;\n"
	"}\n"
	"\n"
	"void main() {\n"
	"    vec2 half_size = size * 0.5;\n"
	"    vec2 p = (v_texcoord * size) - half_size;\n"
	"    float dist = rounded_box_sdf(p, half_size, radius);\n"
	"    if (dist >= 0.5) {\n"
	"        gl_FragColor = texture2D(tex_bg, v_texcoord);\n"
	"    } else if (dist <= -0.5) {\n"
	"        if (has_blur > 0.5) {\n"
	"            gl_FragColor = texture2D(tex_blur, v_texcoord);\n"
	"        } else {\n"
	"            discard;\n"
	"        }\n"
	"    } else {\n"
	"        float alpha = clamp(0.5 - dist, 0.0, 1.0);\n"
	"        vec4 bg = texture2D(tex_bg, v_texcoord);\n"
	"        if (has_blur > 0.5) {\n"
	"            vec4 blur = texture2D(tex_blur, v_texcoord);\n"
	"            gl_FragColor = mix(bg, blur, alpha);\n"
	"        } else {\n"
	"            gl_FragColor = vec4(bg.rgb, 1.0 - alpha);\n"
	"        }\n"
	"    }\n"
	"}\n";

static const char *vertex_shader_src =
	"attribute vec2 position;\n"
	"attribute vec2 texcoord;\n"
	"uniform mat3 proj;\n"
	"varying vec2 v_texcoord;\n"
	"varying vec2 v_pos;\n"
	"void main() {\n"
	"    v_texcoord = texcoord;\n"
	"    v_pos = position;\n"
	"    vec3 p = proj * vec3(position, 1.0);\n"
	"    gl_Position = vec4(p.xy, 0.0, 1.0);\n"
	"}\n";

static const char *frag_shader_2d_src =
	"precision mediump float;\n"
	"varying vec2 v_texcoord;\n"
	"uniform sampler2D tex;\n"
	"uniform vec2 size;\n"
	"uniform float radius;\n"
	"uniform float alpha;\n"
	"\n"
	"float rounded_box_sdf(vec2 p, vec2 half_size, float r) {\n"
	"    vec2 d = abs(p) - half_size + vec2(r);\n"
	"    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;\n"
	"}\n"
	"\n"
	"void main() {\n"
	"    vec2 half_size = size * 0.5;\n"
	"    vec2 p = (v_texcoord * size) - half_size;\n"
	"    float dist = rounded_box_sdf(p, half_size, radius);\n"
	"    float edge_alpha = clamp(0.5 - dist, 0.0, 1.0);\n"
	"    if (edge_alpha <= 0.0) {\n"
	"        discard;\n"
	"    }\n"
	"    vec4 c = texture2D(tex, v_texcoord);\n"
	"    gl_FragColor = c * (alpha * edge_alpha);\n"
	"}\n";

static const char *frag_shader_external_src =
	"#extension GL_OES_EGL_image_external : require\n"
	"precision mediump float;\n"
	"varying vec2 v_texcoord;\n"
	"uniform samplerExternalOES tex;\n"
	"uniform vec2 size;\n"
	"uniform float radius;\n"
	"uniform float alpha;\n"
	"\n"
	"float rounded_box_sdf(vec2 p, vec2 half_size, float r) {\n"
	"    vec2 d = abs(p) - half_size + vec2(r);\n"
	"    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;\n"
	"}\n"
	"\n"
	"void main() {\n"
	"    vec2 half_size = size * 0.5;\n"
	"    vec2 p = (v_texcoord * size) - half_size;\n"
	"    float dist = rounded_box_sdf(p, half_size, radius);\n"
	"    float edge_alpha = clamp(0.5 - dist, 0.0, 1.0);\n"
	"    if (edge_alpha <= 0.0) {\n"
	"        discard;\n"
	"    }\n"
	"    vec4 c = texture2D(tex, v_texcoord);\n"
	"    gl_FragColor = c * (alpha * edge_alpha);\n"
	"}\n";

static GLuint
compile_shader(GLenum type, const char *src)
{
	GLuint shader = glCreateShader(type);
	if (!shader) {
		wlr_log(WLR_ERROR, "glCreateShader failed");
		return 0;
	}
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);

	GLint status;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status) {
		GLint len = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
		char *log = calloc(len + 1, sizeof(char));
		if (log) {
			glGetShaderInfoLog(shader, len, NULL, log);
			wlr_log(WLR_ERROR, "Shader compilation error: %s", log);
			free(log);
		}
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static bool
link_program(GLuint *prog_out, const char *vert_src, const char *frag_src)
{
	GLuint vert = compile_shader(GL_VERTEX_SHADER, vert_src);
	if (!vert) {
		return false;
	}
	GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
	if (!frag) {
		glDeleteShader(vert);
		return false;
	}

	GLuint prog = glCreateProgram();
	glAttachShader(prog, vert);
	glAttachShader(prog, frag);
	glLinkProgram(prog);

	glDeleteShader(vert);
	glDeleteShader(frag);

	GLint status;
	glGetProgramiv(prog, GL_LINK_STATUS, &status);
	if (!status) {
		GLint len = 0;
		glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
		char *log = calloc(len + 1, sizeof(char));
		if (log) {
			glGetProgramInfoLog(prog, len, NULL, log);
			wlr_log(WLR_ERROR, "Program link error: %s", log);
			free(log);
		}
		glDeleteProgram(prog);
		return false;
	}

	*prog_out = prog;
	return true;
}

static bool
init_shader_prog(struct shader_prog *sp, const char *vert_src, const char *frag_src)
{
	if (!link_program(&sp->program, vert_src, frag_src)) {
		return false;
	}
	sp->a_position = glGetAttribLocation(sp->program, "position");
	sp->a_texcoord = glGetAttribLocation(sp->program, "texcoord");
	sp->u_proj = glGetUniformLocation(sp->program, "proj");
	sp->u_tex = glGetUniformLocation(sp->program, "tex");
	sp->u_size = glGetUniformLocation(sp->program, "size");
	sp->u_radius = glGetUniformLocation(sp->program, "radius");
	sp->u_alpha = glGetUniformLocation(sp->program, "alpha");
	return true;
}

static bool
init_clip_prog(struct clip_prog *cp, const char *vert_src, const char *frag_src)
{
	if (!link_program(&cp->program, vert_src, frag_src)) {
		return false;
	}
	cp->a_position = glGetAttribLocation(cp->program, "position");
	cp->u_proj = glGetUniformLocation(cp->program, "proj");
	cp->u_box_pos = glGetUniformLocation(cp->program, "box_pos");
	cp->u_box_size = glGetUniformLocation(cp->program, "box_size");
	cp->u_radius = glGetUniformLocation(cp->program, "radius");
	return true;
}

static bool
init_blur_down_prog(struct blur_down_prog *bp, const char *vert_src, const char *frag_src)
{
	if (!link_program(&bp->program, vert_src, frag_src)) {
		return false;
	}
	bp->a_position = glGetAttribLocation(bp->program, "position");
	bp->a_texcoord = glGetAttribLocation(bp->program, "texcoord");
	bp->u_proj = glGetUniformLocation(bp->program, "proj");
	bp->u_tex = glGetUniformLocation(bp->program, "tex");
	bp->u_halfpixel = glGetUniformLocation(bp->program, "halfpixel");
	bp->u_offset = glGetUniformLocation(bp->program, "offset");
	return true;
}

static bool
init_blur_up_prog(struct blur_up_prog *bp, const char *vert_src, const char *frag_src)
{
	if (!link_program(&bp->program, vert_src, frag_src)) {
		return false;
	}
	bp->a_position = glGetAttribLocation(bp->program, "position");
	bp->a_texcoord = glGetAttribLocation(bp->program, "texcoord");
	bp->u_proj = glGetUniformLocation(bp->program, "proj");
	bp->u_tex = glGetUniformLocation(bp->program, "tex");
	bp->u_halfpixel = glGetUniformLocation(bp->program, "halfpixel");
	bp->u_offset = glGetUniformLocation(bp->program, "offset");
	return true;
}

static bool
init_blur_composite_prog(struct blur_composite_prog *bp, const char *vert_src, const char *frag_src)
{
	if (!link_program(&bp->program, vert_src, frag_src)) {
		return false;
	}
	bp->a_position = glGetAttribLocation(bp->program, "position");
	bp->a_texcoord = glGetAttribLocation(bp->program, "texcoord");
	bp->u_proj = glGetUniformLocation(bp->program, "proj");
	bp->u_tex_blur = glGetUniformLocation(bp->program, "tex_blur");
	bp->u_tex_bg = glGetUniformLocation(bp->program, "tex_bg");
	bp->u_size = glGetUniformLocation(bp->program, "size");
	bp->u_radius = glGetUniformLocation(bp->program, "radius");
	bp->u_has_blur = glGetUniformLocation(bp->program, "has_blur");
	return true;
}

static void
mat3_ortho(float *m, float left, float right, float bottom, float top)
{
	float rl = right - left;
	float tb = top - bottom;

	m[0] = 2.0f / rl;
	m[1] = 0.0f;
	m[2] = 0.0f;

	m[3] = 0.0f;
	m[4] = 2.0f / tb;
	m[5] = 0.0f;

	m[6] = -(right + left) / rl;
	m[7] = -(top + bottom) / tb;
	m[8] = 1.0f;
}

bool
gl_effects_init(struct wlr_renderer *renderer)
{
	if (gl_ctx.initialized) {
		return true;
	}
	if (!renderer || !wlr_renderer_is_gles2(renderer)) {
		wlr_log(WLR_INFO, "GL effects: renderer is not GLES2, skipping shader init");
		return false;
	}

	gl_ctx.egl = wlr_gles2_renderer_get_egl(renderer);
	if (!gl_ctx.egl) {
		wlr_log(WLR_ERROR, "Failed to get EGL from GLES2 renderer");
		return false;
	}

	EGLDisplay dpy = wlr_egl_get_display(gl_ctx.egl);
	EGLContext ctx = wlr_egl_get_context(gl_ctx.egl);
	EGLDisplay prev_dpy = eglGetCurrentDisplay();
	EGLContext prev_ctx = eglGetCurrentContext();
	EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
	EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);

	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		wlr_log(WLR_ERROR, "Failed to make EGL context current");
		return false;
	}

	if (!init_shader_prog(&gl_ctx.prog_2d, vertex_shader_src, frag_shader_2d_src)) {
		wlr_log(WLR_ERROR, "Failed to initialize 2D rounded shader");
		goto err_restore_egl;
	}

	gl_ctx.has_external_oes = wlr_gles2_renderer_check_ext(renderer, "GL_OES_EGL_image_external");
	if (gl_ctx.has_external_oes) {
		if (!init_shader_prog(&gl_ctx.prog_external, vertex_shader_src, frag_shader_external_src)) {
			wlr_log(WLR_INFO, "External OES shader not compiled; continuing without OES shaders");
			gl_ctx.has_external_oes = false;
		}
	}

	if (!init_clip_prog(&gl_ctx.prog_clip, clip_vert_src, clip_frag_src)) {
		wlr_log(WLR_ERROR, "Failed to initialize clip shader");
		goto err_restore_egl;
	}

	if (!init_blur_down_prog(&gl_ctx.prog_blur_down, vertex_shader_src, blur_down_frag_src)) {
		wlr_log(WLR_ERROR, "Failed to initialize blur downsample shader");
		goto err_restore_egl;
	}

	if (!init_blur_up_prog(&gl_ctx.prog_blur_up, vertex_shader_src, blur_up_frag_src)) {
		wlr_log(WLR_ERROR, "Failed to initialize blur upsample shader");
		goto err_restore_egl;
	}

	if (!init_blur_composite_prog(&gl_ctx.prog_blur_composite, vertex_shader_src, blur_composite_frag_src)) {
		wlr_log(WLR_ERROR, "Failed to initialize blur composite shader");
		goto err_restore_egl;
	}

	glGenBuffers(1, &gl_ctx.quad_vbo);

	gl_ctx.initialized = true;
	wlr_log(WLR_INFO, "GL effects: shaders and blur pipeline successfully initialized");

	eglMakeCurrent(prev_dpy, prev_draw, prev_read, prev_ctx);
	return true;

err_restore_egl:
	eglMakeCurrent(prev_dpy, prev_draw, prev_read, prev_ctx);
	return false;
}

void
gl_effects_finish(void)
{
	if (!gl_ctx.initialized) {
		return;
	}
	if (gl_ctx.egl) {
		EGLDisplay dpy = wlr_egl_get_display(gl_ctx.egl);
		EGLContext ctx = wlr_egl_get_context(gl_ctx.egl);
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);

		if (gl_ctx.prog_2d.program) {
			glDeleteProgram(gl_ctx.prog_2d.program);
		}
		if (gl_ctx.prog_external.program) {
			glDeleteProgram(gl_ctx.prog_external.program);
		}
		if (gl_ctx.prog_clip.program) {
			glDeleteProgram(gl_ctx.prog_clip.program);
		}
		if (gl_ctx.prog_blur_down.program) {
			glDeleteProgram(gl_ctx.prog_blur_down.program);
		}
		if (gl_ctx.prog_blur_up.program) {
			glDeleteProgram(gl_ctx.prog_blur_up.program);
		}
		if (gl_ctx.prog_blur_composite.program) {
			glDeleteProgram(gl_ctx.prog_blur_composite.program);
		}
		for (int i = 0; i <= MAX_BLUR_PASSES; i++) {
			if (gl_ctx.fbo_chain[i].texture) {
				glDeleteTextures(1, &gl_ctx.fbo_chain[i].texture);
			}
			if (gl_ctx.fbo_chain[i].fbo) {
				glDeleteFramebuffers(1, &gl_ctx.fbo_chain[i].fbo);
			}
		}
		if (gl_ctx.fbo_bg.texture) {
			glDeleteTextures(1, &gl_ctx.fbo_bg.texture);
		}
		if (gl_ctx.fbo_bg.fbo) {
			glDeleteFramebuffers(1, &gl_ctx.fbo_bg.fbo);
		}
		if (gl_ctx.quad_vbo) {
			glDeleteBuffers(1, &gl_ctx.quad_vbo);
		}
	}
	memset(&gl_ctx, 0, sizeof(gl_ctx));
}

bool
gl_effects_is_available(void)
{
	return gl_ctx.initialized;
}

bool
gl_effects_render_texture_rounded(
	struct wlr_renderer *renderer,
	struct wlr_buffer *dst_buffer,
	struct wlr_texture *texture,
	const struct wlr_fbox *src_box,
	const struct wlr_box *dst_box,
	float corner_radius,
	float alpha,
	enum wl_output_transform transform)
{
	if (!gl_ctx.initialized || !renderer || !dst_buffer || !texture) {
		return false;
	}
	if (!wlr_renderer_is_gles2(renderer) || !wlr_texture_is_gles2(texture)) {
		return false;
	}

	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(renderer, dst_buffer);
	if (!fbo) {
		return false;
	}

	struct wlr_gles2_texture_attribs attribs;
	wlr_gles2_texture_get_attribs(texture, &attribs);

	struct shader_prog *prog = &gl_ctx.prog_2d;
	if (attribs.target == GL_TEXTURE_EXTERNAL_OES) {
		if (!gl_ctx.has_external_oes || !gl_ctx.prog_external.program) {
			return false;
		}
		prog = &gl_ctx.prog_external;
	}

	EGLDisplay dpy = wlr_egl_get_display(gl_ctx.egl);
	EGLContext ctx = wlr_egl_get_context(gl_ctx.egl);
	EGLDisplay prev_dpy = eglGetCurrentDisplay();
	EGLContext prev_ctx = eglGetCurrentContext();
	EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
	EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);

	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		return false;
	}

	GLint prev_fbo;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);

	glViewport(0, 0, dst_buffer->width, dst_buffer->height);

	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(prog->program);

	float proj[9];
	mat3_ortho(proj, 0.0f, (float)dst_buffer->width, (float)dst_buffer->height, 0.0f);
	glUniformMatrix3fv(prog->u_proj, 1, GL_FALSE, proj);

	float x1 = (float)dst_box->x;
	float y1 = (float)dst_box->y;
	float x2 = x1 + (float)dst_box->width;
	float y2 = y1 + (float)dst_box->height;

	float s1 = src_box ? (float)src_box->x / texture->width : 0.0f;
	float t1 = src_box ? (float)src_box->y / texture->height : 0.0f;
	float s2 = src_box ? (float)(src_box->x + src_box->width) / texture->width : 1.0f;
	float t2 = src_box ? (float)(src_box->y + src_box->height) / texture->height : 1.0f;

	float vertices[] = {
		/* pos_x, pos_y, tex_u, tex_v */
		x1, y1, s1, t1,
		x2, y1, s2, t1,
		x1, y2, s1, t2,
		x2, y2, s2, t2,
	};

	glBindBuffer(GL_ARRAY_BUFFER, gl_ctx.quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

	glEnableVertexAttribArray(prog->a_position);
	glEnableVertexAttribArray(prog->a_texcoord);
	glVertexAttribPointer(prog->a_position, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
	glVertexAttribPointer(prog->a_texcoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(attribs.target, attribs.tex);
	glUniform1i(prog->u_tex, 0);

	glUniform2f(prog->u_size, (float)dst_box->width, (float)dst_box->height);
	glUniform1f(prog->u_radius, corner_radius);
	glUniform1f(prog->u_alpha, alpha);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(prog->a_position);
	glDisableVertexAttribArray(prog->a_texcoord);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
	eglMakeCurrent(prev_dpy, prev_draw, prev_read, prev_ctx);

	return true;
}

bool
gl_effects_clip_view_corners(
	struct wlr_renderer *renderer,
	struct wlr_buffer *dst_buffer,
	const struct wlr_box *box,
	float corner_radius)
{
	if (!gl_ctx.initialized || !renderer || !dst_buffer || !box || corner_radius <= 0.0f) {
		return false;
	}
	if (!wlr_renderer_is_gles2(renderer)) {
		return false;
	}

	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(renderer, dst_buffer);
	if (!fbo) {
		return false;
	}

	EGLDisplay dpy = wlr_egl_get_display(gl_ctx.egl);
	EGLContext ctx = wlr_egl_get_context(gl_ctx.egl);
	EGLDisplay prev_dpy = eglGetCurrentDisplay();
	EGLContext prev_ctx = eglGetCurrentContext();
	EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
	EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);

	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		return false;
	}

	GLint prev_fbo;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);

	glViewport(0, 0, dst_buffer->width, dst_buffer->height);

	glEnable(GL_BLEND);
	glBlendFunc(GL_ZERO, GL_SRC_ALPHA);

	glUseProgram(gl_ctx.prog_clip.program);

	float proj[9];
	mat3_ortho(proj, 0.0f, (float)dst_buffer->width, (float)dst_buffer->height, 0.0f);
	glUniformMatrix3fv(gl_ctx.prog_clip.u_proj, 1, GL_FALSE, proj);

	glUniform2f(gl_ctx.prog_clip.u_box_pos, (float)box->x, (float)box->y);
	glUniform2f(gl_ctx.prog_clip.u_box_size, (float)box->width, (float)box->height);
	glUniform1f(gl_ctx.prog_clip.u_radius, corner_radius);

	float x1 = (float)box->x;
	float y1 = (float)box->y;
	float x2 = x1 + (float)box->width;
	float y2 = y1 + (float)box->height;

	float vertices[] = {
		x1, y1,
		x2, y1,
		x1, y2,
		x2, y2,
	};

	glBindBuffer(GL_ARRAY_BUFFER, gl_ctx.quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

	glEnableVertexAttribArray(gl_ctx.prog_clip.a_position);
	glVertexAttribPointer(gl_ctx.prog_clip.a_position, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(gl_ctx.prog_clip.a_position);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
	eglMakeCurrent(prev_dpy, prev_draw, prev_read, prev_ctx);

	return true;
}

static void
ensure_fbo_struct(struct blur_fbo *fbo_item, int width, int height)
{
	if (!fbo_item) {
		return;
	}
	if (fbo_item->texture && fbo_item->width == width && fbo_item->height == height) {
		return;
	}

	if (!fbo_item->fbo) {
		glGenFramebuffers(1, &fbo_item->fbo);
	}
	if (!fbo_item->texture) {
		glGenTextures(1, &fbo_item->texture);
	}

	glBindTexture(GL_TEXTURE_2D, fbo_item->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	glBindFramebuffer(GL_FRAMEBUFFER, fbo_item->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_item->texture, 0);

	fbo_item->width = width;
	fbo_item->height = height;
}

static void
ensure_blur_fbo(int idx, int width, int height)
{
	if (idx < 0 || idx > MAX_BLUR_PASSES) {
		return;
	}
	ensure_fbo_struct(&gl_ctx.fbo_chain[idx], width, height);
}

bool
gl_effects_capture_background(
	struct wlr_renderer *renderer,
	struct wlr_buffer *target_buffer,
	const struct wlr_box *box)
{
	if (!gl_ctx.initialized || !renderer || !target_buffer || !box || box->width <= 0 || box->height <= 0) {
		return false;
	}
	if (!wlr_renderer_is_gles2(renderer)) {
		return false;
	}

	GLuint main_fbo = wlr_gles2_renderer_get_buffer_fbo(renderer, target_buffer);
	if (!main_fbo) {
		return false;
	}

	EGLDisplay dpy = wlr_egl_get_display(gl_ctx.egl);
	EGLContext ctx = wlr_egl_get_context(gl_ctx.egl);
	EGLDisplay prev_dpy = eglGetCurrentDisplay();
	EGLContext prev_ctx = eglGetCurrentContext();
	EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
	EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);

	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		return false;
	}

	GLint prev_fbo;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);

	int box_w = box->width;
	int box_h = box->height;

	ensure_fbo_struct(&gl_ctx.fbo_bg, box_w, box_h);

	glBindFramebuffer(GL_FRAMEBUFFER, main_fbo);
	glBindTexture(GL_TEXTURE_2D, gl_ctx.fbo_bg.texture);
	int gl_y = target_buffer->height - (box->y + box->height);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, box->x, gl_y, box_w, box_h);

	glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
	eglMakeCurrent(prev_dpy, prev_draw, prev_read, prev_ctx);
	return true;
}

bool
gl_effects_apply_dual_kawase_blur(
	struct wlr_renderer *renderer,
	struct wlr_buffer *target_buffer,
	const struct wlr_box *box,
	float corner_radius,
	int passes,
	float blur_radius,
	bool blur_enabled)
{
	if (!gl_ctx.initialized || !renderer || !target_buffer || !box) {
		return false;
	}
	if (!wlr_renderer_is_gles2(renderer) || box->width <= 0 || box->height <= 0) {
		return false;
	}

	if (passes <= 0) {
		passes = 3;
	}
	if (passes > MAX_BLUR_PASSES) {
		passes = MAX_BLUR_PASSES;
	}
	if (blur_radius <= 0.0f) {
		blur_radius = 5.0f;
	}

	GLuint main_fbo = wlr_gles2_renderer_get_buffer_fbo(renderer, target_buffer);
	if (!main_fbo) {
		return false;
	}

	EGLDisplay dpy = wlr_egl_get_display(gl_ctx.egl);
	EGLContext ctx = wlr_egl_get_context(gl_ctx.egl);
	EGLDisplay prev_dpy = eglGetCurrentDisplay();
	EGLContext prev_ctx = eglGetCurrentContext();
	EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
	EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);

	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		return false;
	}

	GLint prev_fbo;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);

	int box_w = box->width;
	int box_h = box->height;

	/* If background was not captured beforehand, capture from current main_fbo */
	if (!gl_ctx.fbo_bg.texture || gl_ctx.fbo_bg.width != box_w || gl_ctx.fbo_bg.height != box_h) {
		ensure_fbo_struct(&gl_ctx.fbo_bg, box_w, box_h);
		glBindFramebuffer(GL_FRAMEBUFFER, main_fbo);
		glBindTexture(GL_TEXTURE_2D, gl_ctx.fbo_bg.texture);
		int gl_y = target_buffer->height - (box->y + box->height);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, box->x, gl_y, box_w, box_h);
	}

	/* Prepare quad vertices for offscreen FBO rendering */
	float quad_verts[] = {
		-1.0f, -1.0f, 0.0f, 0.0f,
		 1.0f, -1.0f, 1.0f, 0.0f,
		-1.0f,  1.0f, 0.0f, 1.0f,
		 1.0f,  1.0f, 1.0f, 1.0f,
	};
	glBindBuffer(GL_ARRAY_BUFFER, gl_ctx.quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad_verts), quad_verts, GL_DYNAMIC_DRAW);

	float identity_proj[9] = {
		1.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	};

	if (blur_enabled) {
		glDisable(GL_BLEND);

		/* 1. Downsample Passes from fbo_bg into chain */
		glUseProgram(gl_ctx.prog_blur_down.program);
		glUniformMatrix3fv(gl_ctx.prog_blur_down.u_proj, 1, GL_FALSE, identity_proj);
		glEnableVertexAttribArray(gl_ctx.prog_blur_down.a_position);
		glEnableVertexAttribArray(gl_ctx.prog_blur_down.a_texcoord);
		glVertexAttribPointer(gl_ctx.prog_blur_down.a_position, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
		glVertexAttribPointer(gl_ctx.prog_blur_down.a_texcoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

		int cur_w = box_w;
		int cur_h = box_h;

		for (int i = 0; i < passes; i++) {
			int next_w = MAX(cur_w / 2, 1);
			int next_h = MAX(cur_h / 2, 1);
			ensure_blur_fbo(i + 1, next_w, next_h);

			glBindFramebuffer(GL_FRAMEBUFFER, gl_ctx.fbo_chain[i + 1].fbo);
			glViewport(0, 0, next_w, next_h);

			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, (i == 0) ? gl_ctx.fbo_bg.texture : gl_ctx.fbo_chain[i].texture);
			glUniform1i(gl_ctx.prog_blur_down.u_tex, 0);

			glUniform2f(gl_ctx.prog_blur_down.u_halfpixel, 0.5f / (float)cur_w, 0.5f / (float)cur_h);
			glUniform1f(gl_ctx.prog_blur_down.u_offset, blur_radius);

			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

			cur_w = next_w;
			cur_h = next_h;
		}

		glDisableVertexAttribArray(gl_ctx.prog_blur_down.a_position);
		glDisableVertexAttribArray(gl_ctx.prog_blur_down.a_texcoord);

		/* 2. Upsample Passes back to chain[0] */
		glUseProgram(gl_ctx.prog_blur_up.program);
		glUniformMatrix3fv(gl_ctx.prog_blur_up.u_proj, 1, GL_FALSE, identity_proj);
		glEnableVertexAttribArray(gl_ctx.prog_blur_up.a_position);
		glEnableVertexAttribArray(gl_ctx.prog_blur_up.a_texcoord);
		glVertexAttribPointer(gl_ctx.prog_blur_up.a_position, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
		glVertexAttribPointer(gl_ctx.prog_blur_up.a_texcoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

		ensure_blur_fbo(0, box_w, box_h);

		for (int i = passes; i > 0; i--) {
			int target_idx = i - 1;
			int target_w = (target_idx == 0) ? box_w : gl_ctx.fbo_chain[target_idx].width;
			int target_h = (target_idx == 0) ? box_h : gl_ctx.fbo_chain[target_idx].height;

			glBindFramebuffer(GL_FRAMEBUFFER, gl_ctx.fbo_chain[target_idx].fbo);
			glViewport(0, 0, target_w, target_h);

			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, gl_ctx.fbo_chain[i].texture);
			glUniform1i(gl_ctx.prog_blur_up.u_tex, 0);

			glUniform2f(gl_ctx.prog_blur_up.u_halfpixel, 0.5f / (float)gl_ctx.fbo_chain[i].width, 0.5f / (float)gl_ctx.fbo_chain[i].height);
			glUniform1f(gl_ctx.prog_blur_up.u_offset, blur_radius);

			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}

		glDisableVertexAttribArray(gl_ctx.prog_blur_up.a_position);
		glDisableVertexAttribArray(gl_ctx.prog_blur_up.a_texcoord);
	}

	/* 3. Composite back into main target buffer */
	glBindFramebuffer(GL_FRAMEBUFFER, main_fbo);
	glViewport(0, 0, target_buffer->width, target_buffer->height);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(gl_ctx.prog_blur_composite.program);

	float proj[9];
	mat3_ortho(proj, 0.0f, (float)target_buffer->width, (float)target_buffer->height, 0.0f);
	glUniformMatrix3fv(gl_ctx.prog_blur_composite.u_proj, 1, GL_FALSE, proj);

	float x1 = (float)box->x;
	float y1 = (float)box->y;
	float x2 = x1 + (float)box_w;
	float y2 = y1 + (float)box_h;

	float comp_vertices[] = {
		x1, y1, 0.0f, 1.0f,
		x2, y1, 1.0f, 1.0f,
		x1, y2, 0.0f, 0.0f,
		x2, y2, 1.0f, 0.0f,
	};

	glBindBuffer(GL_ARRAY_BUFFER, gl_ctx.quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(comp_vertices), comp_vertices, GL_DYNAMIC_DRAW);

	glEnableVertexAttribArray(gl_ctx.prog_blur_composite.a_position);
	glEnableVertexAttribArray(gl_ctx.prog_blur_composite.a_texcoord);
	glVertexAttribPointer(gl_ctx.prog_blur_composite.a_position, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
	glVertexAttribPointer(gl_ctx.prog_blur_composite.a_texcoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

	if (blur_enabled && gl_ctx.fbo_chain[0].texture) {
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, gl_ctx.fbo_chain[0].texture);
		glUniform1i(gl_ctx.prog_blur_composite.u_tex_blur, 0);
	}

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, gl_ctx.fbo_bg.texture);
	glUniform1i(gl_ctx.prog_blur_composite.u_tex_bg, 1);

	glUniform2f(gl_ctx.prog_blur_composite.u_size, (float)box_w, (float)box_h);
	glUniform1f(gl_ctx.prog_blur_composite.u_radius, corner_radius);
	glUniform1f(gl_ctx.prog_blur_composite.u_has_blur, blur_enabled ? 1.0f : 0.0f);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(gl_ctx.prog_blur_composite.a_position);
	glDisableVertexAttribArray(gl_ctx.prog_blur_composite.a_texcoord);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
	eglMakeCurrent(prev_dpy, prev_draw, prev_read, prev_ctx);

	return true;
}

static void
render_scene_buffer_rounded_tree(
	struct wlr_renderer *renderer,
	struct wlr_buffer *dst_buffer,
	struct wlr_scene_node *node,
	int parent_x, int parent_y,
	const struct wlr_box *dst_box,
	float corner_radius,
	float alpha)
{
	if (!node || !node->enabled) {
		return;
	}
	switch (node->type) {
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			render_scene_buffer_rounded_tree(renderer, dst_buffer, child,
				parent_x + node->x, parent_y + node->y,
				dst_box, corner_radius, alpha);
		}
		break;
	}
	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
		if (!scene_buffer->buffer) {
			break;
		}
		struct wlr_texture *texture = NULL;
		struct wlr_client_buffer *client_buffer = wlr_client_buffer_get(scene_buffer->buffer);
		if (client_buffer) {
			texture = client_buffer->texture;
		}
		if (!texture) {
			break;
		}
		struct wlr_box surface_dst = {
			.x = parent_x + node->x,
			.y = parent_y + node->y,
			.width = scene_buffer->dst_width,
			.height = scene_buffer->dst_height,
		};
		wlr_log(WLR_DEBUG, "[gl-effects] Drawing client sub-texture at (%d,%d %dx%d) radius=%.1f",
			surface_dst.x, surface_dst.y, surface_dst.width, surface_dst.height, corner_radius);

		gl_effects_render_texture_rounded(
			renderer,
			dst_buffer,
			texture,
			&scene_buffer->src_box,
			&surface_dst,
			corner_radius,
			alpha * scene_buffer->opacity,
			scene_buffer->transform);
		break;
	}
	default:
		break;
	}
}

bool
gl_effects_render_view_content(
	struct wlr_renderer *renderer,
	struct wlr_buffer *dst_buffer,
	struct view *view,
	int offset_x,
	int offset_y,
	float corner_radius,
	float alpha)
{
	if (!view || !view->content_tree) {
		return false;
	}
	struct wlr_box view_box = {
		.x = view->current.x + offset_x,
		.y = view->current.y + offset_y,
		.width = view->current.width,
		.height = view->current.height,
	};
	wlr_log(WLR_DEBUG, "[gl-effects] rendering rounded view '%s' at (%d,%d %dx%d) radius=%.1f",
		view->title ? view->title : "view", view_box.x, view_box.y, view_box.width, view_box.height, corner_radius);

	render_scene_buffer_rounded_tree(
		renderer,
		dst_buffer,
		&view->content_tree->node,
		view_box.x,
		view_box.y,
		&view_box,
		corner_radius,
		alpha);
	return true;
}

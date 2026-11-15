// A layer of flat drawing over the 3D picture: boxes, lines, dots and text, in
// pixels from the top left of the window. Plain OpenGL (3.3), beside GameApi: it
// draws after GameApi has drawn the scene, and leaves OpenGL as it found it.
//
// The letters are glyphs.hpp (DejaVu Sans Mono, written by tools/font.sh).
#pragma once
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "glyphs.hpp"

struct colour { float r, g, b, a; };

class panels {
 public:
  int width = 0, height = 0;  // of the window, from OpenGL, at start()

  bool init() {
    const char* vs =
        "#version 330 core\n"
        "layout(location = 0) in vec2 at; layout(location = 1) in vec2 glyph; layout(location = 2) in vec4 paint;\n"
        "uniform vec2 window; out vec2 uv; out vec4 col;\n"
        "void main() { uv = glyph; col = paint; gl_Position = vec4(at.x / window.x * 2.0 - 1.0, 1.0 - at.y / window.y * 2.0, 0.0, 1.0); }\n";
    const char* fs =
        "#version 330 core\n"
        "in vec2 uv; in vec4 col; uniform sampler2D letters; out vec4 o;\n"
        "void main() { float a = uv.x < 0.0 ? 1.0 : texture(letters, uv).r; o = vec4(col.rgb, col.a * a); }\n";
    const auto compile = [](GLenum kind, const char* text) {
      const GLuint s = glCreateShader(kind);
      glShaderSource(s, 1, &text, nullptr);
      glCompileShader(s);
      GLint ok = 0;
      glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
      if (!ok) {
        char log[400];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "panels: %s\n", log);
      }
      return s;
    };
    program_ = glCreateProgram();
    glAttachShader(program_, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(program_, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(program_);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) return false;
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    // the letters: all 95 in one row
    const int n = 95;
    std::vector<unsigned char> px(static_cast<std::size_t>(n * kGlyphW * kGlyphH));
    for (int g = 0; g < n; ++g)
      for (int y = 0; y < kGlyphH; ++y)
        for (int x = 0; x < kGlyphW; ++x) {
          const char c = kGlyphs[g * kGlyphW * kGlyphH + y * kGlyphW + x];
          px[static_cast<std::size_t>(y * n * kGlyphW + g * kGlyphW + x)] = static_cast<unsigned char>(17 * (c <= '9' ? c - '0' : c - 'a' + 10));
        }
    GLint bound = 0, align = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    glGenTextures(1, &letters_);
    glBindTexture(GL_TEXTURE_2D, letters_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, n * kGlyphW, kGlyphH, 0, GL_RED, GL_UNSIGNED_BYTE, px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
    return true;
  }

  // call first in every frame: how large the window is
  void start() {
    GLint view[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, view);
    width = view[2];
    height = view[3];
  }

  void box(float x, float y, float w, float h, colour c) {
    put(x, y, c), put(x + w, y, c), put(x, y + h, c);
    put(x + w, y, c), put(x + w, y + h, c), put(x, y + h, c);
  }
  void frame(float x, float y, float w, float h, colour c) {
    box(x, y, w, 1, c), box(x, y + h - 1, w, 1, c), box(x, y, 1, h, c), box(x + w - 1, y, 1, h, c);
  }
  void line(float x0, float y0, float x1, float y1, float thick, colour c) {
    const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6f) return;
    const float nx = -dy / len * thick * 0.5f, ny = dx / len * thick * 0.5f;
    put(x0 + nx, y0 + ny, c), put(x1 + nx, y1 + ny, c), put(x0 - nx, y0 - ny, c);
    put(x1 + nx, y1 + ny, c), put(x1 - nx, y1 - ny, c), put(x0 - nx, y0 - ny, c);
  }
  void dot(float x, float y, float r, colour c) {
    const int n = r < 4 ? 10 : r < 20 ? 24 : 64;
    for (int i = 0; i < n; ++i) {
      const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(n), b = 6.2831853f * static_cast<float>(i + 1) / static_cast<float>(n);
      put(x, y, c), put(x + r * std::cos(a), y + r * std::sin(a), c), put(x + r * std::cos(b), y + r * std::sin(b), c);
    }
  }
  void ring(float x, float y, float r, float thick, colour c) {
    const int n = r < 20 ? 24 : 72;
    for (int i = 0; i < n; ++i) {
      const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(n), b = 6.2831853f * static_cast<float>(i + 1) / static_cast<float>(n);
      line(x + r * std::cos(a), y + r * std::sin(a), x + r * std::cos(b), y + r * std::sin(b), thick, c);
    }
  }
  // text with its top left at (x, y); every letter kGlyphW wide and kGlyphH high
  void text(float x, float y, const std::string& s, colour c) {
    x = std::floor(x), y = std::floor(y);
    for (const char ch : s) {
      if (ch > 32 && ch < 127) {
        const float u0 = static_cast<float>(ch - 32) / 95.0f, u1 = static_cast<float>(ch - 31) / 95.0f;
        const auto w = static_cast<float>(kGlyphW), h = static_cast<float>(kGlyphH);
        put(x, y, c, u0, 0), put(x + w, y, c, u1, 0), put(x, y + h, c, u0, 1);
        put(x + w, y, c, u1, 0), put(x + w, y + h, c, u1, 1), put(x, y + h, c, u0, 1);
      }
      x += static_cast<float>(kGlyphW);
    }
  }
  static float text_width(const std::string& s) { return static_cast<float>(s.size()) * static_cast<float>(kGlyphW); }

  // Draws what has been put since the last call; with a rectangle, only inside it.
  void draw(const float* inside = nullptr) {
    if (v_.empty()) return;
    GLint program = 0, vao = 0, buffer = 0, unit = 0, texture = 0, src = 0, dst = 0, src_a = 0, dst_a = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_BLEND_SRC_RGB, &src);
    glGetIntegerv(GL_BLEND_DST_RGB, &dst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);
    const GLboolean depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND), cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (inside) {
      glEnable(GL_SCISSOR_TEST);
      glScissor(static_cast<GLint>(inside[0]), height - static_cast<GLint>(inside[1] + inside[3]), static_cast<GLsizei>(inside[2]), static_cast<GLsizei>(inside[3]));
    } else {
      glDisable(GL_SCISSOR_TEST);
    }
    glUseProgram(program_);
    glUniform2f(glGetUniformLocation(program_, "window"), static_cast<float>(width), static_cast<float>(height));
    glUniform1i(glGetUniformLocation(program_, "letters"), 0);
    glBindTexture(GL_TEXTURE_2D, letters_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v_.size() * sizeof(float)), v_.data(), GL_STREAM_DRAW);
    const GLsizei stride = 8 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(4 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v_.size() / 8));
    v_.clear();

    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(buffer));
    glBindVertexArray(static_cast<GLuint>(vao));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
    glActiveTexture(static_cast<GLenum>(unit));
    glUseProgram(static_cast<GLuint>(program));
    glBlendFuncSeparate(static_cast<GLenum>(src), static_cast<GLenum>(dst), static_cast<GLenum>(src_a), static_cast<GLenum>(dst_a));
    (depth ? glEnable : glDisable)(GL_DEPTH_TEST);
    (blend ? glEnable : glDisable)(GL_BLEND);
    (cull ? glEnable : glDisable)(GL_CULL_FACE);
    (scissor ? glEnable : glDisable)(GL_SCISSOR_TEST);
  }

 private:
  void put(float x, float y, colour c, float u = -1.0f, float v = -1.0f) {
    for (const float f : {x, y, u, v, c.r, c.g, c.b, c.a}) v_.push_back(f);
  }
  GLuint program_ = 0, vao_ = 0, vbo_ = 0, letters_ = 0;
  std::vector<float> v_;
};

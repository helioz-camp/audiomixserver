#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>
#include <unordered_map>
#include <limits>

#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/Importer.hpp>

#include "SDL.h"
#include "SDL_mixer.h"

#include "boost/program_options.hpp"

#include "event2/buffer.h"
#include "event2/event.h"
#include "event2/event_compat.h"
#include "event2/event_struct.h"
#include "event2/http.h"
#include "event2/http_compat.h"
#include "event2/keyvalq_struct.h"

#include "GL/glew.h"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/ext.hpp>
#include <glm/gtx/string_cast.hpp>

namespace {
typedef uint64_t sequence_t;

std::unordered_map<char, std::string> const &character_to_morse{
    {'\n', ".-.-"},   {' ', " "},      {'!', "---."},   {'\"', ".-..-."},
    {'\'', ".----."}, {'(', "-.--."},  {')', "-.--.-"}, {'+', ".-.-."},
    {',', "--..--"},  {'-', "-....-"}, {'.', ".-.-.-"}, {'/', "-..-."},
    {'0', "-----"},   {'1', ".----"},  {'2', "..---"},  {'3', "...--"},
    {'4', "....-"},   {'5', "....."},  {'6', "-...."},  {'7', "--..."},
    {'8', "---.."},   {'9', "----."},  {':', "---..."}, {';', "-.-.-."},
    {'=', "-...-"},   {'?', "..--.."}, {'@', ".--.-."}, {'A', ".-"},
    {'B', "-..."},    {'C', "-.-."},   {'D', "-.."},    {'E', "."},
    {'F', "..-."},    {'G', "--."},    {'H', "...."},   {'I', ".."},
    {'J', ".---"},    {'K', "-.-"},    {'L', ".-.."},   {'M', "--"},
    {'N', "-."},      {'O', "---"},    {'P', ".--."},   {'Q', "--.-"},
    {'R', ".-."},     {'S', "..."},    {'T', "-"},      {'U', "..-"},
    {'V', "...-"},    {'W', ".--"},    {'X', "-..-"},   {'Y', "-.--"},
    {'Z', "--.."},
};

void clear_gl_errors_helper(char const *function, int line) {
  auto err = glGetError();

  if (err != GL_NO_ERROR) {
    std::cerr << __FILE__ << ":" << line << " in " << function
              << " check_gl_error " << err << " " << gluErrorString(err) << std::endl;
    clear_gl_errors_helper(function, line);
  }
}

#define clear_gl_errors() clear_gl_errors_helper(__FUNCTION__, __LINE__)

struct helio_gl_shader {
  GLuint gl_shader_number;
  std::string shader_source_string;
  std::string const shader_filename;
  GLenum gl_shader_type;

  helio_gl_shader(std::string const &filename, GLenum shader_type)
      : gl_shader_number(glCreateShader(shader_type)),
        shader_filename(filename), gl_shader_type(shader_type) {
    read_shader_source_string();
  }

  void read_shader_source_string() {
    std::ostringstream oss;
    std::ifstream s{shader_filename};
    if (!s.good()) {
      std::cerr << "read_shader_source_string failed to read "
                << shader_filename << std::endl;
    }

    oss << s.rdbuf();
    shader_source_string = oss.str();
  }

  void compile_shader() {
    GLchar const *source = shader_source_string.c_str();
    GLint sizes[] = {GLint(shader_source_string.size())};
    glShaderSource(gl_shader_number, 1, &source, sizes);

    glCompileShader(gl_shader_number);
    clear_gl_errors();

    GLsizei returned_size = 0;
    GLchar info_log[0x1000];
    GLint was_compiled = GL_FALSE;
    glGetShaderiv(gl_shader_number, GL_COMPILE_STATUS, &was_compiled);
    glGetShaderInfoLog(gl_shader_number, sizeof(info_log) / sizeof(info_log[0]),
                       &returned_size, info_log);
    if (returned_size) {
      std::cerr << "GL compile shader " << shader_filename << ": " << info_log
                << std::endl;
    }

    if (was_compiled != GL_TRUE) {
      std::cerr << "Could not compile shader from " << shader_filename
                << std::endl
                << shader_source_string << std::endl;
    }
  }
};

struct helio_gl_program {
  GLuint gl_program_number;
  std::vector<helio_gl_shader> gl_program_shaders;

  void init_gl_program() { gl_program_number = glCreateProgram(); }

  void reset_shader(helio_gl_shader &shader) {
    glDetachShader(gl_program_number, shader.gl_shader_number);

    compile_and_attach_shader(shader);
  }

  template <typename... Params> void add_shader(Params &&... params) {
    gl_program_shaders.emplace_back(params...);
    compile_and_attach_shader(gl_program_shaders.back());
  }

  void compile_and_attach_shader(helio_gl_shader &shader) {
    shader.compile_shader();
    glAttachShader(gl_program_number, shader.gl_shader_number);
  }

  void link_gl_program() {
    glLinkProgram(gl_program_number);
    GLsizei returned_size = 0;
    GLchar info_log[0x1000];
    glGetProgramInfoLog(gl_program_number,
                        sizeof(info_log) / sizeof(info_log[0]), &returned_size,
                        info_log);
    if (returned_size) {
      std::cerr << "GL link_gl_program notes " << info_log << std::endl;
    }

    GLint status = GL_FALSE;
    glGetProgramiv(gl_program_number, GL_LINK_STATUS, &status);

    if (status != GL_TRUE) {
      std::cerr << "GL link_gl_program failed " << gl_program_number
                << std::endl;
    }
    clear_gl_errors();
  }
};

struct helio_gl_lozenge 
{
  helio_gl_program gl_program;
  GLuint position_attrib_number;
  GLuint lozenge_center_uniform_number;
  GLuint lozenge_size_uniform_number;
  GLuint lozenge_color_uniform_number;
  GLuint lozenge_body_width_uniform_number;
  GLuint vertex_buffer_number;
  GLint gl_viewport_dimensions[4];
  float display_ratio;
  
  float const body_radius = 0.03;
  float const vertical_spacing = 0.02;
  std::string lozenge_message; 
  
  void init_lozenge() {
    clear_gl_errors();

    GLfloat vertex_buffer_data[] = {
        -1, -1, -1, +1, +1, +1, +1, +1, +1, -1, -1, -1,
    };
    gl_program.init_gl_program();
    gl_program.add_shader("lozenge_gl_vertex.glsl", GL_VERTEX_SHADER);
    gl_program.add_shader("lozenge_gl_fragment.glsl", GL_FRAGMENT_SHADER);
    gl_program.link_gl_program();

    position_attrib_number =
        glGetAttribLocation(gl_program.gl_program_number, "position");
    lozenge_center_uniform_number =
        glGetUniformLocation(gl_program.gl_program_number, "lozenge_center");
    lozenge_size_uniform_number =
        glGetUniformLocation(gl_program.gl_program_number, "lozenge_size");
    lozenge_body_width_uniform_number =
        glGetUniformLocation(gl_program.gl_program_number, "lozenge_body_width");
    lozenge_color_uniform_number =
        glGetUniformLocation(gl_program.gl_program_number, "lozenge_color");

    glGenBuffers(1, &vertex_buffer_number);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_number);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_buffer_data),
                 vertex_buffer_data, GL_STATIC_DRAW);
    glGetIntegerv(GL_VIEWPORT, gl_viewport_dimensions);
    display_ratio = gl_viewport_dimensions[2] / float(gl_viewport_dimensions[3]);
    clear_gl_errors();
  }

  void render_lozenges() {
    glUseProgram(gl_program.gl_program_number);
    glEnableVertexAttribArray(position_attrib_number);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_number);
    glVertexAttribPointer(position_attrib_number, 2, GL_FLOAT, GL_FALSE, 0,
                          nullptr);

    uint32_t unsigned_colors[] = { 0xEF5A5C, 0xFFB259, 0xFFDD4A, 0x83CA76, 0x6698F2, 0x9473C8 };
    unsigned color_index = 0;
    float left_x = -1;
    float top_y = 1;
    float gap_spacing = 0.5 * body_radius;
    float extra_word_spacing = 3.5 * body_radius;
    
    float dash_body_width = 2;
    std::istringstream iss{lozenge_message};
    std::string word;
    while (iss >> word) {
      float word_width = (word.size()-1) * gap_spacing
        + 2 * std::count(word.begin(), word.end(), '-') * dash_body_width * body_radius
        + 2 * word.size() * body_radius;
      if (left_x != -1 && word_width + left_x > 1.0) {
        left_x = -1;
        top_y -= (vertical_spacing + 2 * body_radius) * display_ratio;      
      }
      
      for (auto c : word) {
        float body_width = (c == '-') ? dash_body_width : 0;
        auto width = (body_width + 1) * 2 * body_radius;
        render_lozenge_position(
                                left_x + width/2,
                                top_y - body_radius*display_ratio,
                                unsigned_colors[color_index],
                                body_width);

        left_x += width + gap_spacing;
      }
      left_x += extra_word_spacing;

      if (++color_index >= sizeof(unsigned_colors)/sizeof(unsigned_colors[0])) {
        color_index = 0;
      }
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }

  void render_lozenge_position(float x, float y, uint32_t unsigned_color, float body_width) {
    glUniform2f(lozenge_center_uniform_number, x, y);
    glUniform2f(lozenge_size_uniform_number, body_radius * (1 + body_width), body_radius * display_ratio);
    glUniform1f(lozenge_body_width_uniform_number, body_width);

    glUniform3f(lozenge_color_uniform_number,
                (0xff & (unsigned_color >> 16))/255.,
                (0xff & (unsigned_color >> 8))/255.,
                (0xff & (unsigned_color >> 0))/255.
                );

    glDrawArrays(GL_TRIANGLES, 0, 6);
    clear_gl_errors();
  }
  
};
  
  
struct helio_gl_rainbow {
  helio_gl_program gl_program;
  GLuint position_attrib_number;
  GLuint background_uniform_number;
  GLuint wobble_uniform_number;
  GLuint resolution_uniform_number;
  GLuint vertex_buffer_number;
  GLuint fire_start_uniform_number;
  float fire_start = 0;

  void init_rainbow() {
    clear_gl_errors();

    GLfloat vertex_buffer_data[] = {
        -1, -1, -1, +1, +1, +1, +1, +1, +1, -1, -1, -1,
    };
    gl_program.init_gl_program();
    gl_program.add_shader("rainbow_gl_vertex.glsl", GL_VERTEX_SHADER);
    gl_program.add_shader("rainbow_gl_fragment.glsl", GL_FRAGMENT_SHADER);
    gl_program.link_gl_program();

    position_attrib_number =
        glGetAttribLocation(gl_program.gl_program_number, "position");
    background_uniform_number =
        glGetUniformLocation(gl_program.gl_program_number, "background");
    wobble_uniform_number =
        glGetUniformLocation(gl_program.gl_program_number, "wobble");
    resolution_uniform_number =
        glGetUniformLocation(gl_program.gl_program_number, "resolution");
    fire_start_uniform_number =
        glGetUniformLocation(gl_program.gl_program_number, "fire_start");

    glGenBuffers(1, &vertex_buffer_number);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_number);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_buffer_data),
                 vertex_buffer_data, GL_STATIC_DRAW);

    glUseProgram(gl_program.gl_program_number);

    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    glUniform2f(resolution_uniform_number, vp[2], vp[3]);
    clear_gl_errors();
  }

  void render_rainbow(float background_r, float background_g,
                      float background_b) {
    glUseProgram(gl_program.gl_program_number);
    glEnableVertexAttribArray(position_attrib_number);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_number);
    glVertexAttribPointer(position_attrib_number, 2, GL_FLOAT, GL_FALSE, 0,
                          nullptr);
    glUniform1f(
        wobble_uniform_number,
        std::fmod(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                          .count() *
                      0.001,
                  M_PI * 2));
    glUniform1f(fire_start_uniform_number, fire_start);
    fire_start *= .99;
    if (fire_start < 0.001) {
      fire_start = 0;
    }

    glUniform3f(background_uniform_number, background_r, background_g,
                background_b);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    clear_gl_errors();
  }
};

struct helio_sprite {
  GLuint sprite_vertex_buffer_number;
  GLuint sprite_vertices_count;
  std::vector<glm::vec3> sprite_vertices;
  
  void load_sprite_into_gl() {
    clear_gl_errors();

    glGenBuffers(1, &sprite_vertex_buffer_number);
    glBindBuffer(GL_ARRAY_BUFFER, sprite_vertex_buffer_number);
    glBufferData(GL_ARRAY_BUFFER,
                 sizeof(sprite_vertices[0])*sprite_vertices.size(),
                 &sprite_vertices[0], GL_STATIC_DRAW);
    sprite_vertices_count = sprite_vertices.size();
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    clear_gl_errors();
  }

  void render_one_sprite(GLuint sprite_position_attrib_number) {
    clear_gl_errors();
    glEnableVertexAttribArray(sprite_position_attrib_number);
    glBindBuffer(GL_ARRAY_BUFFER, sprite_vertex_buffer_number);
    glVertexAttribPointer(sprite_position_attrib_number, 3, GL_FLOAT, GL_FALSE, 0, 0);
    
    glDrawArrays(GL_TRIANGLES, 0, sprite_vertices_count);
    clear_gl_errors();
  }
};

struct helio_gl_sprites {
  helio_gl_program gl_program;
  std::vector<helio_sprite> active_sprites;
  GLuint sprite_position_attrib_number;
  GLuint sprite_model_uniform_number;
  GLuint sprite_view_uniform_number;
  GLuint sprite_projection_uniform_number;
  GLuint sprite_color_uniform_number;
  float display_ratio;

  void init_sprites() {
    clear_gl_errors();
    gl_program.init_gl_program();
    gl_program.add_shader("sprite_gl_vertex.glsl", GL_VERTEX_SHADER);
    gl_program.add_shader("sprite_gl_fragment.glsl", GL_FRAGMENT_SHADER);
    gl_program.link_gl_program();
    sprite_position_attrib_number = 
        glGetAttribLocation(gl_program.gl_program_number, "sprite_position");
    sprite_model_uniform_number =
        glGetUniformLocation(gl_program.gl_program_number, "sprite_model");
    sprite_view_uniform_number =
        glGetUniformLocation(gl_program.gl_program_number, "sprite_view");
    sprite_projection_uniform_number =
        glGetUniformLocation(gl_program.gl_program_number, "sprite_projection");
    sprite_color_uniform_number =
        glGetUniformLocation(gl_program.gl_program_number, "sprite_color");

    clear_gl_errors();

    for (auto &sprite : active_sprites) {
      sprite.load_sprite_into_gl();
    }
    
    GLint gl_viewport_dimensions[4];
    glGetIntegerv(GL_VIEWPORT, gl_viewport_dimensions);
    display_ratio = gl_viewport_dimensions[2] / float(gl_viewport_dimensions[3]);
    clear_gl_errors();
  }

  void render_sprites() {
    clear_gl_errors();
    if (active_sprites.empty()) {
      return;
    }

    glUseProgram(gl_program.gl_program_number);

    glm::mat4 model = glm::rotate(
                                  glm::mat4(1.0f),
                                  (float)std::fmod(
                                                   std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                                                         std::chrono::system_clock::now().time_since_epoch())
                                                   .count() *
                                                   0.001,
                                                   M_PI * 2),
                                  glm::vec3(-1.0f,-1.0f,1.0f));
    
    glUniformMatrix4fv(sprite_model_uniform_number, 1, GL_FALSE,
                       glm::value_ptr(model));

    glUniformMatrix4fv(sprite_view_uniform_number, 1, GL_FALSE,
                       glm::value_ptr(
                                      glm::lookAt(
                                                  glm::vec3(30.0f,30.0f,30.0f),
                                                  glm::vec3(0,0,0),
                                                  glm::vec3(0,1,0)
                                                  ))
                       );
    glUniformMatrix4fv(sprite_projection_uniform_number, 1, GL_FALSE, glm::value_ptr(
                                                                                     glm::perspective(
                                                                                                      glm::radians(60.0f), 
                                                                                                      display_ratio,    
                                                                                                      0.1f,             
                                                                                                      1000.0f          
                                                                                                      )));
    unsigned unsigned_color = 0xff00ff;
    
    glUniform3f(sprite_color_uniform_number,
                (0xff & (unsigned_color >> 16))/255.,
                (0xff & (unsigned_color >> 8))/255.,
                (0xff & (unsigned_color >> 0))/255.
                );
    
    for (auto &sprite: active_sprites) {
      //      std::cout << "render_sprite " << glm::to_string(model) << " " << glm::to_string(model*glm::vec4(sprite.sprite_vertices[200], 1.0f)) << " count " << sprite.sprite_vertices_count << std::endl;
      sprite.render_one_sprite(sprite_position_attrib_number);
    }

    clear_gl_errors();    
  }
};

struct sequence_status {
  Mix_Chunk *sequence_chunk;
  int sequence_channel;
  sequence_t next_sequence;
  float sequence_brightness;

  sequence_status(Mix_Chunk *chunk)
      : sequence_chunk(chunk), sequence_channel(-1), next_sequence(0),
        sequence_brightness(0) {}
};

std::unordered_map<std::string, std::string> uri_params(evhttp_uri const *uri) {
  struct evkeyvalq params;
  std::unordered_map<std::string, std::string> ret;

  if (evhttp_parse_query_str(evhttp_uri_get_query(uri), &params)) {
    return ret;
  }
  for (auto kv = params.tqh_first; kv; kv = kv->next.tqe_next) {
    if (kv->key) {
      ret[kv->key] = kv->value;
    }
  }

  evhttp_clear_headers(&params);

  return ret;
}

std::chrono::time_point<std::chrono::system_clock> start =
    std::chrono::system_clock::now();
long time_millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now() - start)
      .count();
}
sequence_t random_sequence_number() {
  static auto rnd = std::mt19937(); // std::random_device()());
  static std::uniform_int_distribution<sequence_t> dist;
  return dist(rnd);
}

bool starts_with(std::string const &prefix, std::string const &str) {
  return str.compare(0, prefix.size(), prefix) == 0;
}

struct lock_sdl_audio {
  lock_sdl_audio() { SDL_LockAudio(); }
  ~lock_sdl_audio() { SDL_UnlockAudio(); }
};

struct context {
  std::unordered_map<std::string, Mix_Chunk *> chunks;
  std::unordered_map<std::string, uint64_t> client_tokens;
  std::vector<Mix_Chunk *> ordered_chunks;
  boost::program_options::variables_map &vm;
  struct event udp_event;

  std::unordered_map<int, sequence_t> channel_to_sequence;
  std::unordered_map<sequence_t, sequence_status> sequence_to_status;
  sequence_t sequence = random_sequence_number();

  GLclampf background_r;
  GLclampf background_g;
  GLclampf background_b;

  helio_gl_rainbow gl_rainbow;
  helio_gl_lozenge gl_lozenge;
  helio_gl_sprites gl_sprites;

  context(boost::program_options::variables_map &vm_)
      : vm(vm_), background_r(0), background_g(0), background_b(0) {}

  context(const context&) = delete;

  Mix_Chunk *name_to_chunk(std::string const &name) {
    auto p = chunks.find(name);

    if (ordered_chunks.empty()) {
      std::cout << "No songs loaded - pass them at the command line"
                << std::endl;
      return 0;
    }
    if (p != chunks.end()) {
      std::cerr << "Chunk " << name << " is " << p->second << std::endl;
      return p->second;
    }

    if (name == "play" || name == "/play" || name.empty()) {
      return ordered_chunks[0];
    }

    unsigned long index;
    try {
      index = std::stoul(name);
    } catch (std::exception &e) {
      std::cout << "Unknown song " << name << std::endl;
      return 0;
    }
    return ordered_chunks[index % ordered_chunks.size()];
  }

  sequence_t play(std::string const &name) {
    auto chunk = name_to_chunk(name);
    if (!chunk) {
      return 0;
    }
    return play(chunk);
  }

  sequence_t play_morse(std::string const &morse) {
    gl_lozenge.lozenge_message = morse;
    gl_rainbow.fire_start = 1;
    
    auto dot = load_to_chunk("morse_dot.wav");
    auto dash = load_to_chunk("morse_dash.wav");
    auto space = load_to_chunk("morse_space.wav");
    auto gap = load_to_chunk("morse_gap.wav");

    lock_sdl_audio _;
    sequence_status *last_status = nullptr;
    sequence_t first_sequence = 0;
    auto add_chunk = [&](Mix_Chunk *chunk, double brightness = 0) {
      if (!chunk) {
        return;
      }

      auto i = sequence_to_status
                   .emplace(fresh_sequence_number(), sequence_status{chunk})
                   .first;
      i->second.sequence_brightness = brightness;
      if (last_status) {
        last_status->next_sequence = i->first;
      }
      if (!first_sequence) {
        first_sequence = i->first;
      }

      last_status = &i->second;
    };

    for (auto const &c : morse) {
      Mix_Chunk *chunk = nullptr;
      auto brightness = 0.0;
      switch (c) {
      case '.':
        chunk = dot;
        brightness = .9;
        break;
      case '-':
        chunk = dash;
        brightness = 1.0;
        break;
      case ' ':
        chunk = space;
        break;
      }
      add_chunk(chunk, brightness);
      add_chunk(gap);
    }

    if (!first_sequence) {
      return 0;
    }

    return start_sequence(sequence_to_status.find(first_sequence));
  }

  sequence_t fresh_sequence_number() {
    auto new_sequence = ++sequence;
    if (0 == sequence) {
      new_sequence = ++sequence;
    }
    return new_sequence;
  }

  sequence_t play(Mix_Chunk *chunk) {
    lock_sdl_audio _;
    auto i = sequence_to_status.emplace(fresh_sequence_number(),
                                        sequence_status{chunk});
    return start_sequence(i.first);
  }

  void set_brightness(float brightness) {

    if (vm["flash_screen"].as<bool>()) {
      background_r = brightness;
      background_g = brightness;
      background_b = brightness;
    }

    auto laser_level = brightness > 0.5 ? 1 : 0;

    auto option = vm["gpio_path"];
    if (!option.empty()) {
      std::ofstream laser_gpio{option.as<std::string>()};
      laser_gpio << (laser_level ^ vm["gpio_off_value"].as<int>());
    }
  }

  sequence_t start_sequence(decltype(sequence_to_status)::iterator const &i) {
    int channel = Mix_PlayChannel(-1, i->second.sequence_chunk, 0);
    if (channel < 0) {
      std::cerr << "Mix_PlayChannel " << channel << " " << Mix_GetError()
                << " for sequence " << i->first << std::endl;
      sequence_done(i->first);
      return 0;
    } else {
      std::cout << time_millis() << " playing " << i->first << " on channel "
                << channel << std::endl;
    }

    set_brightness(i->second.sequence_brightness);

    channel_to_sequence[channel] = i->first;
    i->second.sequence_channel = channel;
    return i->first;
  }

  void handle_http_request(evhttp_request *req) {
    char *address;
    ev_uint16_t port;
    struct evhttp_connection *con = evhttp_request_get_connection(req);
    evhttp_connection_get_peer(con, &address, &port);

    auto uri = evhttp_request_get_evhttp_uri(req);

    auto path = std::string(evhttp_uri_get_path(uri));
    std::cout << "Received HTTP request from " << address << ":" << port
              << " for " << path << std::endl;
    std::ostringstream out;

    bool success = handle_request(out, uri);

    auto *buf = evhttp_request_get_output_buffer(req);
    if (!buf) {
      std::cerr << "evhttp_request_get_output_buffer" << std::endl;
      return;
    }
    auto str = out.str();
    if (evbuffer_add(buf, str.data(), str.length())) {
      std::cerr << "evbuffer_add" << std::endl;
      return;
    }

    evhttp_send_reply(req, success ? HTTP_OK : 500, "Rock on", buf);
  }

  void handle_udp_events(evutil_socket_t sock) {
    struct sockaddr_storage addr;
    char buf[1 << 16];
    for (;;) {
      socklen_t addr_len = sizeof(addr);
      auto bytes = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&addr,
                            &addr_len);
      if (bytes < 0) {
        return;
      }

      handle_udp_request(sock, std::string(buf, bytes), &addr, addr_len);
    }
  }

  std::string remote_address(const void *addr, int addr_len) {
    switch (static_cast<const sockaddr *>(addr)->sa_family) {
    case AF_INET: {
      char namebuf[INET_ADDRSTRLEN];
      auto sa = static_cast<const sockaddr_in *>(addr);
      if (!evutil_inet_ntop(AF_INET, &sa->sin_addr, namebuf, sizeof(namebuf))) {
        std::cerr << "evutil_inet_ntop AF_INET "
                  << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR())
                  << std::endl;
        return "<UNKNOWN-IPv4>";
      }

      return std::string(namebuf) + ":" + std::to_string(ntohs(sa->sin_port));
    }
    case AF_INET6: {
      char namebuf[INET6_ADDRSTRLEN];
      auto sa = static_cast<const sockaddr_in6 *>(addr);
      if (!evutil_inet_ntop(AF_INET6, &sa->sin6_addr, namebuf,
                            sizeof(namebuf))) {
        std::cerr << "evutil_inet_ntop AF_INET6 "
                  << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR())
                  << std::endl;
        return "<UNKNOWN-IPv6>";
      }

      return std::string(namebuf) + ":" + std::to_string(ntohs(sa->sin6_port));
    }
    default:
      return "<UNKNOWN-AF>";
    }
  }

  bool handle_request(std::ostream &out, evhttp_uri const *uri) {
    timeval tv;
    if (evutil_gettimeofday(&tv, nullptr) < 0) {
      std::cerr << "evutil_gettimeofday "
                << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR())
                << std::endl;
      std::memset(&tv, 0, sizeof(tv));
    }

    auto path = evhttp_uri_get_path(uri);
    while (path && *path == '/')
      ++path;
    auto cmd = std::string(path);
    auto params = uri_params(uri);
    auto get_sequence = [&]() -> sequence_t {
      try {
        return std::stoull(params["sequence"]);
      } catch (std::exception &e) {
        std::cerr << "Parse failed for " << path << ": " << e.what()
                  << std::endl;
        out << "NO SEQUENCE " << path << std::endl;
        return 0;
      }
    };

    out << "TIME " << tv.tv_sec << "." << std::setw(5) << std::setfill('0')
        << tv.tv_usec << std::endl;
    if ("ping" == cmd || "reset" == cmd) {
      out << "PONG" << std::endl << params["payload"] << std::endl;
      return true;
    } else if ("stop" == cmd) {
      auto sequence = get_sequence();
      {
        lock_sdl_audio _;
        auto i = sequence_to_status.find(sequence);
        if (i != sequence_to_status.end()) {
          if (i->second.sequence_channel < 0) {
            if (i->second.next_sequence) {
              std::cerr << "Unplayed sequence " << i->first
                        << " has next sequence " << i->second.next_sequence
                        << std::endl;
            }

            sequence_to_status.erase(i);
          } else {
            Mix_HaltChannel(i->second.sequence_channel);
          }
        }
      }
      out << "STOPPED" << std::endl;
      return true;
    } else if ("queue" == cmd) {
      auto sequence = get_sequence();
      if (!sequence) {
        return false;
      }
      auto chunk = name_to_chunk(params["sample"]);
      if (!chunk) {
        out << "NO SAMPLE" << std::endl;
        return false;
      }

      sequence_t seq = 0;
      bool found = false;

      {
        lock_sdl_audio _;
        auto i = sequence_to_status.find(sequence);
        if (i != sequence_to_status.end()) {
          found = true;
          if (i->second.sequence_channel >= 0) {
            sequence_done(i->second.next_sequence);
            i->second.next_sequence = seq = fresh_sequence_number();
            sequence_to_status.emplace(seq, sequence_status{chunk});
          }
        }
      }
      if (seq) {
        out << "QUEUED " << seq << std::endl;
        return true;
      } else if (found) {
        out << "WAIT" << std::endl;
        return true;
      } else if (auto s = play(chunk)) {
        out << "PLAYING " << s << std::endl;
        return true;
      } else {
        out << "FAILED" << std::endl;
        return false;
      }
    } else if ("songs" == cmd) {
      out << "SONGS " << ordered_chunks.size() << std::endl;
      for (auto const &pair : chunks) {
        out << pair.first << std::endl;
      }
      return true;
    } else if (cmd.empty()) {
      for (auto const &pair : chunks) {
        auto encoded = std::unique_ptr<char, decltype(&free)>(
            evhttp_uriencode(pair.first.data(), pair.first.size(), true),
            &free);
        out << "<A href=\"/play?sample=" << encoded.get() << "\">" << pair.first
            << "</a><br/>" << std::endl;
      }
      return true;
    } else if ("song_count" == cmd) {
      out << "SONGS " << ordered_chunks.size() << std::endl;
      return true;
    } else if ("play_morse_message" == cmd) {
      auto message_text = params["message"];
      std::ostringstream oss;
      bool first = true;
      for (auto &c : message_text) {
        if (!first) {
          oss << " ";
        } else {
          first = false;
        }

        auto m = character_to_morse.find(std::toupper(c));
        if (character_to_morse.end() == m) {
          out << "UNKNOWN CHARACTER" << std::endl;
          return false;
        } else {
          oss << m->second;
        }
      }
      std::cout << "sending morse code " << oss.str() << std::endl;

      auto s = play_morse(oss.str());
      out << "PLAYING " << s << std::endl;
      return true;
    } else {
      auto sample = params["sample"];
      if (sample.empty()) {
        sample = evhttp_uri_get_path(uri);
      }
      if (auto sequence = play(sample)) {
        out << "PLAYING " << sequence << std::endl;
        return true;
      } else {
        out << "FAILED" << std::endl;
        return false;
      }
    }
  }

  void handle_udp_request(evutil_socket_t sock, const std::string &buf,
                          const void *addr, int addr_len) {
    std::istringstream in(buf);

    std::string client;
    std::string client_token;
    std::string cmd;
    std::string path;
    std::getline(in, client);
    std::getline(in, client_token);
    std::getline(in, cmd);
    std::getline(in, path);

    std::ostringstream out;

    auto client_token_number = std::stoull(client_token);

    auto remote = remote_address(addr, addr_len);
    std::cout << "Received UDP request from " << remote << " for " << client
              << " with token " << client_token_number << ": " << cmd << " "
              << path << std::endl;

    if (!starts_with("audiomixclient/", client)) {
      std::cerr << "Not an audiomixclient: " << client << std::endl;
      return;
    }

    out << "audiomixserver/3" << std::endl
        << "TOKEN " << client_token_number << std::endl;

    if (client_tokens[remote] >= client_token_number && cmd != "reset") {
      out << "ALREADY " << client_tokens[remote] << std::endl;
    } else {
      client_tokens[remote] = client_token_number;
      auto uri = std::unique_ptr<evhttp_uri, decltype(&evhttp_uri_free)>(
          evhttp_uri_parse(cmd.c_str()), &evhttp_uri_free);
      handle_request(out, uri.get());
    }

    auto msg = out.str();

    if (sendto(sock, msg.data(), msg.size(), 0,
               static_cast<const sockaddr *>(addr),
               addr_len) != ssize_t(msg.size())) {
      std::cerr << "sendto "
                << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR())
                << std::endl;
    }
  }

  bool init_http() {
    // no cleanup, no need
    evhttp *ev_web = evhttp_start(vm["bind_address"].as<std::string>().c_str(),
                                  vm["bind_port"].as<int>());
    if (!ev_web) {
      std::cerr << "evhttp_start "
                << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR())
                << std::endl;
      return false;
    }
    evhttp_set_gencb(ev_web,
                     [](evhttp_request *req, void *ptr) -> void {
                       static_cast<context *>(ptr)->handle_http_request(req);
                     },
                     this);
    return true;
  }

  bool init_udp() {
    // no cleanup, no need
    auto sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
      std::cerr << "socket: " << EVUTIL_SOCKET_ERROR() << std::endl;
      return false;
    }
    if (evutil_make_socket_nonblocking(sock)) {
      std::cerr << "evutil_make_socket_nonblocking "
                << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR())
                << std::endl;
      return false;
    }
    if (evutil_make_listen_socket_reuseable(sock)) {
      std::cerr << "evutil_make_socket_reuseable "
                << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR())
                << std::endl;
      return false;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(vm["bind_port_udp"].as<int>());
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr))) {
      std::cerr << "bind "
                << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR())
                << std::endl;
      return false;
    }

    event_set(&udp_event, sock, EV_READ | EV_PERSIST,
              [](evutil_socket_t sock, short what, void *ctx) -> void {
                static_cast<context *>(ctx)->handle_udp_events(sock);
              },
              this);
    event_add(&udp_event, NULL);

    return true;
  }

  void sequence_done(sequence_t sequence) {
    if (!sequence) {
      return;
    }

    auto i = sequence_to_status.find(sequence);
    if (i != sequence_to_status.end()) {
      auto status = i->second;
      sequence_to_status.erase(i);
      if (status.next_sequence) {
        auto next_status = sequence_to_status.find(status.next_sequence);
        if (next_status != sequence_to_status.end()) {
          std::cout << time_millis() << " queued play of "
                    << status.next_sequence << " after " << sequence
                    << std::endl;
          start_sequence(next_status);
        }
      }
    }
  }

  void finished_channel(int channel) {
    auto sequence = channel_to_sequence[channel];
    std::cout << time_millis() << " finished playing " << sequence
              << " on channel " << channel << std::endl;
    set_brightness(0.);

    channel_to_sequence.erase(channel);
    sequence_done(sequence);
  }

  void setup_opengl_thread() {
    std::cout << "setup_opengl_thread GL " << glGetString(GL_VERSION)
              << std::endl;
    clear_gl_errors();
    gl_rainbow.init_rainbow();
    gl_lozenge.init_lozenge();
    gl_sprites.init_sprites();
  }

  void render_frame_with_opengl() {
    clear_gl_errors();
    glEnable(GL_BLEND);
    //    glEnable(GL_DEPTH_TEST);  
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(background_r, background_g, background_b, 1.0);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

    gl_sprites.render_sprites();
    //    gl_rainbow.render_rainbow(background_r, background_g, background_b);
    gl_lozenge.render_lozenges();

    glFinish();
    clear_gl_errors();
  }

  void maybe_load_file_from_name(std::string const& file) {
    if (chunks.find(file) != chunks.end()) {
      return;
    }
    std::cout << "Loading " << file << std::endl;
    auto chunk = Mix_LoadWAV(file.c_str());
    if (!chunk) {
      std::cerr << "Could not load " << file << ": " << Mix_GetError()
                << std::endl;
      return;
    }
    
    chunks[file] = chunk;
    ordered_chunks.push_back(chunk);
  }

  void load_audio_from_filenames(std::vector<std::string> const& filenames) {  
    for (auto &file : filenames) {
      maybe_load_file_from_name(file);
    }
  }
  void load_3d_models_from_paths(std::vector<std::string> const& filenames) {  
    Assimp::Importer importer;

    for (auto &file : filenames) {
      const aiScene* scene = importer.ReadFile(file,
            aiProcess_GenSmoothNormals |
            aiProcess_CalcTangentSpace |
            aiProcess_Triangulate |
            aiProcess_JoinIdenticalVertices |
            aiProcess_SortByPType);
      if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE ||
          !scene->mRootNode) {
        std::cerr << "load_3d_models_from_paths failed " << file << " error "
                  << importer.GetErrorString() << std::endl;
        continue;
      }
      std::cout << "load_3d_models_from_paths " <<file << " meshes " << scene->mNumMeshes << std::endl;
      std::vector<glm::vec3> output_vertices;
      glm::vec3 min(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
      glm::vec3 max(std::numeric_limits<float>::min(), std::numeric_limits<float>::min(), std::numeric_limits<float>::min());
      for (unsigned n = 0; scene->mNumMeshes > n; ++n) {
        auto mesh = scene->mMeshes[n];

        for (unsigned f = 0; mesh->mNumFaces > f; ++f) {
          auto const&face = mesh->mFaces[f];
          for (unsigned i = 0; face.mNumIndices > i; ++i) {
            auto const&p = mesh->mVertices[face.mIndices[i]];
            glm::vec3 g(p.x, p.y, p.z);
            min = glm::min(min, g);
            max = glm::max(max, g);
            output_vertices.push_back(g);
          }
        }
      }
      std::cout << "meshes " << scene->mNumMeshes << " output_vertices " << output_vertices.size() << " min " << glm::to_string(min) << " max " << glm::to_string(max) << std::endl;
      helio_sprite sprite{.sprite_vertices = std::move(output_vertices)};

      gl_sprites.active_sprites.emplace_back(std::move(sprite));      
      
    }
  }

  Mix_Chunk *load_to_chunk(std::string const &name) {
    maybe_load_file_from_name(name);

    return name_to_chunk(name);
  }
};

context *global_ctx;

void finished_channel(int channel) { global_ctx->finished_channel(channel); }
} // namespace

int main(int argc, char *argv[]) {
  namespace po = boost::program_options;
  po::options_description description("Mix audio");
  description.add_options()("help", "Display this help message")(
      "frequency", po::value<int>()->default_value(44100), "Frequency in Hz")(
      "channels", po::value<int>()->default_value(2), "Channels")(
      "chunksize", po::value<int>()->default_value(512),
      "Bytes sent to sound output each time, divide by frequency to find "
      "duration")("sample-files", po::value<std::vector<std::string>>(),
                  "OGG, WAV or MP3 sample files")(
      "gpio_path", po::value<std::string>(),
      "path to GPIO pin for the laser relay")(
      "gpio_off_value", po::value<int>()->default_value(1),
      "value for the GPIO pin when the laser is off")(
      "allocate_sdl_channels", po::value<int>()->default_value(2048),
      "Number of SDL channels to mix together")(
      "bind_address", po::value<std::string>()->default_value("0.0.0.0"),
      "Address to listen on for HTTP")("bind_port",
                                       po::value<int>()->default_value(13231),
                                       "Port to listen on for HTTP")(
      "bind_port_udp", po::value<int>()->default_value(13231),
      "Port to listen on for UDP")("visuals",
                                   po::value<bool>()->default_value(true),
                                   "Open GL visualisations")
                                   ("flash_screen",
                                    po::value<bool>()->default_value(false), "Turn screen white when playing morse")
    ("3d-model-paths", po::value<std::vector<std::string>>(), "paths to 3D model files");

  po::variables_map vm;
  po::positional_options_description p;
  p.add("sample-files", -1);
  po::store(po::command_line_parser(argc, argv)
                .options(description)
                .positional(p)
                .run(),
            vm);
  po::notify(vm);

  if (vm.count("help")) {
    std::cerr << description;
    return 1;
  }

  int ret = SDL_Init(SDL_INIT_AUDIO);

  if (ret < 0) {
    std::cerr << "SDL_Init audio " << ret << " " << SDL_GetError() << std::endl;
    return 2;
  }

  ret = Mix_OpenAudio(vm["frequency"].as<int>(), AUDIO_S16SYS,
                      vm["channels"].as<int>(), vm["chunksize"].as<int>());
  if (ret < 0) {
    std::cerr << "Mix_OpenAudio " << ret << " " << Mix_GetError() << std::endl;
    return 3;
  }

  context ctx(vm);
  Mix_AllocateChannels(vm["allocate_sdl_channels"].as<int>());

  if (vm.count("3d-model-paths")) {
    ctx.load_3d_models_from_paths(vm["3d-model-paths"].as<std::vector<std::string>>());
  }

  if (vm["visuals"].as<bool>()) {
    ret = SDL_Init(SDL_INIT_VIDEO);
    SDL_ShowCursor(SDL_DISABLE);
    // reset control-C handling to default, which is messed up by SDL
    std::signal(SIGINT, SIG_DFL);
    if (ret < 0) {
      std::cerr << "SDL_Init video " << ret << " " << SDL_GetError()
                << std::endl;
      return 2;
    }
    SDL_DisplayMode display_mode;

    SDL_DisableScreenSaver();
    
    ret = SDL_GetCurrentDisplayMode(0, &display_mode);
    if (ret < 0) {
      std::cerr << "SDL_GetCurrentDisplayMode " << ret << " " << SDL_GetError()
                << std::endl;
      return 93;
    }

    new std::thread([&] {
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
      SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
      auto window = SDL_CreateWindow(argv[0], SDL_WINDOWPOS_UNDEFINED,
                                     SDL_WINDOWPOS_UNDEFINED, display_mode.w,
                                     display_mode.h,
                                     SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);

      SDL_GL_CreateContext(window);
      clear_gl_errors();
      auto init_error = glewInit();
      if (GLEW_OK != init_error) {
        std::cerr << "glewInit failed: " << glewGetErrorString(init_error)
                  << std::endl;
      }
      SDL_GL_SetSwapInterval(1);
      ctx.setup_opengl_thread();
      for (;;) {
        ctx.render_frame_with_opengl();
        SDL_GL_SwapWindow(window);
      }
    });
  }

  global_ctx = &ctx; // as finished_channel needs a global
  Mix_ChannelFinished(finished_channel);

  if (vm.count("sample-files")) {
    ctx.load_audio_from_filenames(vm["sample-files"].as<std::vector<std::string>>());
  }

  if (!event_init()) {
    std::cerr << "event_init" << std::endl;
    return 5;
  }

  if (!ctx.init_udp()) {
    std::cerr << "init_udp" << std::endl;
  }
  if (!ctx.init_http()) {
    std::cerr << "init_http" << std::endl;
  }

  auto libevent_thread = std::thread([] {
    if (event_dispatch() == -1) {
      std::cerr << "event_dispatch" << std::endl;
      std::exit(7);
    }
  });

  // SDL demands the main thread under Mac OS X or else gets
  // "nextEventMatchingMask should only be called from the Main
  // Thread!"
  for (;;) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
      case SDL_KEYUP:
        if (event.key.keysym.sym == SDLK_ESCAPE) {
          std::cout << "Exiting due to SDL_KEYUP SDLK_ESCAPE" << std::endl;
          std::exit(0);
        }

        break;
      case SDL_QUIT:
        std::cout << "Exiting due to SDL_QUIT" << std::endl;
        std::exit(0);
      }
    }
  }

  return 0;
}

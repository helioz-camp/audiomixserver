#include <cctype>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <random>
#include <unordered_map>

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

namespace {
typedef unsigned long sequence_t;

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

struct sequence_status {
  Mix_Chunk *sequence_chunk;
  int sequence_channel;
  sequence_t next_sequence;

  sequence_status(Mix_Chunk *chunk)
      : sequence_chunk(chunk), sequence_channel(-1), next_sequence(0) {}
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
  std::unordered_map<std::string, unsigned long> client_tokens;
  std::vector<Mix_Chunk *> ordered_chunks;
  boost::program_options::variables_map &vm;
  struct event udp_event;

  std::unordered_map<int, sequence_t> channel_to_sequence;
  std::unordered_map<sequence_t, sequence_status> sequence_to_status;
  sequence_t sequence = random_sequence_number();

  context(boost::program_options::variables_map &vm_) : vm(vm_) {}

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
    auto dot = name_to_chunk("morse_dot.mp3");
    auto dash = name_to_chunk("morse_dash.mp3");
    auto space = name_to_chunk("morse_space.mp3");

    lock_sdl_audio _;
    sequence_status *last_status = nullptr;
    sequence_t first_sequence = 0;

    for (auto const &c : morse) {
      Mix_Chunk *chunk = nullptr;
      switch (c) {
      case '.':
        chunk = dot;
        break;
      case '-':
        chunk = dash;
        break;
      case ' ':
        chunk = space;
        break;
      }
      if (chunk) {
        auto i = sequence_to_status
                     .emplace(fresh_sequence_number(), sequence_status{chunk})
                     .first;
        if (last_status) {
          last_status->next_sequence = i->first;
        }
        if (!first_sequence) {
          first_sequence = i->first;
        }

        last_status = &i->second;
      }
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
      if (evutil_inet_ntop(AF_INET, &sa->sin_addr, namebuf, sizeof(namebuf)) <
          0) {
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
      if (evutil_inet_ntop(AF_INET6, &sa->sin6_addr, namebuf, sizeof(namebuf)) <
          0) {
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
        return std::stoul(params["sequence"]);
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

    unsigned long client_token_number = std::stoul(client_token);

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

    channel_to_sequence.erase(channel);
    sequence_done(sequence);
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
      "allocate_sdl_channels", po::value<int>()->default_value(2048),
      "Number of SDL channels to mix together")(
      "bind_address", po::value<std::string>()->default_value("0.0.0.0"),
      "Address to listen on for HTTP")("bind_port",
                                       po::value<int>()->default_value(13231),
                                       "Port to listen on for HTTP")(
      "bind_port_udp", po::value<int>()->default_value(13231),
      "Port to listen on for UDP");
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
    std::cerr << "SDL_Init " << ret << " " << SDL_GetError() << std::endl;
    return 2;
  }

  ret = Mix_OpenAudio(vm["frequency"].as<int>(), AUDIO_S16SYS,
                      vm["channels"].as<int>(), vm["chunksize"].as<int>());
  if (ret < 0) {
    std::cerr << "Mix_OpenAudio " << ret << " " << Mix_GetError() << std::endl;
    return 3;
  }

  Mix_AllocateChannels(vm["allocate_sdl_channels"].as<int>());

  context ctx(vm);
  global_ctx = &ctx; // as finished_channel needs a global
  Mix_ChannelFinished(finished_channel);

  if (vm.count("sample-files")) {
    for (auto &file : vm["sample-files"].as<std::vector<std::string>>()) {
      std::cout << "Loading " << file << std::endl;
      auto chunk = Mix_LoadWAV(file.c_str());
      if (!chunk) {
        std::cerr << "Could not load " << file << ": " << Mix_GetError()
                  << std::endl;
        continue;
      }

      ctx.chunks[file] = chunk;
      ctx.ordered_chunks.push_back(chunk);
    }
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

  if (event_dispatch() == -1) {
    std::cerr << "event_dispatch" << std::endl;
    return 7;
  }

  return 0;
}

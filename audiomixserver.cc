#include <iostream>
#include <chrono>
#include <unordered_map>
#include <cstdio>
#include <iomanip>
#include <random>

#include "SDL.h"
#include "SDL_mixer.h"

#include "boost/program_options.hpp"

#include "evhttp.h"

namespace 
{
  typedef unsigned long sequence_t;
  
  std::chrono::time_point<std::chrono::system_clock> start = std::chrono::system_clock::now();
  long time_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now()-start).count();
  }
  sequence_t random_sequence_number() 
  {
    static auto rnd = std::mt19937(std::random_device()());
    static std::uniform_int_distribution<sequence_t> dist;
    return dist(rnd);
  }
  

  struct lock_sdl_audio 
  {
    lock_sdl_audio() 
    {
      SDL_LockAudio();
    }
    ~lock_sdl_audio() 
    {
      SDL_UnlockAudio();      
    }
  };
  

  struct context 
  {
    std::unordered_map<std::string, Mix_Chunk *> chunks;
    std::unordered_map<std::string, unsigned long> client_tokens;
    std::vector<Mix_Chunk*> ordered_chunks;
    boost::program_options::variables_map& vm;
    struct event udp_event;

    std::unordered_map<int, sequence_t> channel_to_sequence;
    std::unordered_map<sequence_t, int> sequence_to_channel;
    std::unordered_map<sequence_t, Mix_Chunk*> sequence_to_next_chunk;
    sequence_t sequence = random_sequence_number();
  
    context(boost::program_options::variables_map& vm_):vm(vm_) {}

    sequence_t play(std::string const& name) {
      auto p = chunks.find(name);

      if (ordered_chunks.empty()) {
	std::cout << "No songs loaded - pass them at the command line" << std::endl;
	return 0;
      }
    
    
      if (p == chunks.end()) {
	if (name == "play" || name.empty()) {
	  return play(ordered_chunks[0]);
	}

	unsigned long index;
	try {
	  index = std::stoul(name);
	} catch (std::exception& e) {
	  std::cout << "Unknown song " << name << std::endl;
	  return 0;
	}

	return play(ordered_chunks[index%ordered_chunks.size()]);
      }

      return play(p->second);
    }

    sequence_t play(Mix_Chunk* chunk) {
      int ret = Mix_PlayChannel(-1, chunk, 0);
      if (ret < 0) {
	std::cerr << "Mix_PlayChannel " << ret << " " << Mix_GetError() << std::endl;
	return 0;
      } else {
	std::cout << time_millis()  << " playing on channel " << ret << std::endl;
      }
    
      auto current_sequence =  ++sequence;
      if (0 == sequence) {
	current_sequence = ++sequence;
      }

      {
	lock_sdl_audio _;
	channel_to_sequence[ret] = current_sequence;
	sequence_to_channel[current_sequence] = ret;
      }
    
      return current_sequence;
    }
  
    void handle_http_request(evhttp_request *req) {
      char *address;
      ev_uint16_t port;
      struct evhttp_connection *con = evhttp_request_get_connection (req);
      evhttp_connection_get_peer(con, &address, &port);
    
      auto *out = evhttp_request_get_output_buffer(req);
      if (!out) {
	std::cerr << "evhttp_request_get_output_buffer" << std::endl;
	return;
      }
      auto uri = evhttp_request_get_evhttp_uri(req);
      auto path = std::string(evhttp_uri_get_path(uri));
      std::cout << "Received HTTP request from " << address << ":" << port << " for " << path << std::endl;

      if ("/" == path) {
	for (auto const& p : chunks) {
	  evbuffer_add_printf(out, "<a href=\"%s\">%s</a>\n", p.first.c_str(), p.first.c_str());
	}
	if (chunks.empty()) {
	  evbuffer_add_printf(out, "No tunes passed on the command line!\n");
	}
      
	evhttp_send_reply(req, HTTP_OK, "Pick a tune", out);
	return;
      }	
    
      if (auto sequence = play(path)) {
	evbuffer_add_printf(out, "<a href=\"%s\">Played sequence %lu</a>", path.c_str(), (unsigned long)sequence);
	evhttp_send_reply(req, HTTP_OK, "Mixing madly", out);
	return;
      }

      evbuffer_add_printf(out, "Not found. <a href=\"/\">Home beat.</a>\n");
      evhttp_send_reply(req, HTTP_NOTFOUND, "Not a tune", out);
    }

    void handle_udp_events(evutil_socket_t sock) {
      struct sockaddr_storage addr;
      char buf[1<<16];
      for (;;) {
	socklen_t addr_len = sizeof(addr);
	auto bytes = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*) &addr, &addr_len);
	if (bytes < 0) {
	  return;
	}
      
	handle_udp_request(sock, std::string(buf, bytes), &addr, addr_len);
      }
    }

    std::string remote_address(const void* addr, int addr_len) {
      switch(static_cast<const sockaddr*>(addr)->sa_family) {
      case AF_INET:
	{
	  char namebuf[INET_ADDRSTRLEN];
	  auto sa = static_cast<const sockaddr_in*>(addr);
	  if (evutil_inet_ntop(AF_INET, &sa->sin_addr,
			       namebuf, sizeof(namebuf)) < 0) {
	    std::cerr << "evutil_inet_ntop AF_INET " << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()) << std::endl;
	    return "<UNKNOWN-IPv4>";
	  }
	
	  return std::string(namebuf) + ":" + std::to_string(ntohs(sa->sin_port));
	}
      case AF_INET6:
	{	
	  char namebuf[INET6_ADDRSTRLEN];
	  auto sa = static_cast<const sockaddr_in6*>(addr);
	  if (evutil_inet_ntop(AF_INET6, &sa->sin6_addr,
			       namebuf, sizeof(namebuf)) < 0) {
	    std::cerr << "evutil_inet_ntop AF_INET6 " << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()) << std::endl;
	    return "<UNKNOWN-IPv6>";
	  }
	
	  return std::string(namebuf)  + ":" + std::to_string(ntohs(sa->sin6_port));
	}
      default:
	return "<UNKNOWN-AF>";
      }
    }

    void handle_request(std::ostream& out, std::string const& cmd, std::string const& path) {
      timeval tv;
      if (evutil_gettimeofday(&tv, nullptr) < 0) {
	std::cerr << "evutil_gettimeofday " << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()) << std::endl;
	std::memset(&tv, 0, sizeof(tv));
      }
      out << "TIME " << tv.tv_sec << "." << std::setw(6) << tv.tv_usec << std::endl;
      if ("ping" == cmd || "reset" == cmd) {
	out << "PONG" << std::endl
	    << path << std::endl;
      } else if ("stop" == cmd) {
	sequence_t sequence;
	try {
	  sequence = std::stoul(path);
	} catch (std::exception& e) {
	  std::cerr << "Parse failed for stop " << path << ": " << e.what() << std::endl;
	  out << "PARSE FAILED " << path << std::endl;
	  return;
	}

	{
	  lock_sdl_audio _;
	  auto i = sequence_to_channel.find(sequence);
	  if (i != sequence_to_channel.end()) {
	    Mix_HaltChannel(i->second);
	  }
	}
	out << "STOPPED" << std::endl;
      } else if ("play" == cmd || path.empty()) {
	if (auto sequence = play("play" == cmd ? path : cmd)) {
	  out << "PLAYING SEQUENCE " << sequence << std::endl;
	} else {
	  out << "FAILED" << std::endl;
	}
      } else {
	out << "UNKNOWN" << std::endl;
      }
    }

    void handle_udp_request(evutil_socket_t sock, const std::string& buf, const void* addr, int addr_len) {
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
      std::cout << "Received UDP request from " << remote << " for " << client << " with token " << client_token_number << std::endl;

      out << "audiomixserver/1" << std::endl
	  << "TOKEN " << client_tokens[remote] << std::endl;
    
      if (client_tokens[remote] >= client_token_number && cmd != "reset"){
	out << "ALREADY AT TOKEN " << client_tokens[remote] << std::endl;
      } else {
	client_tokens[remote] = client_token_number;
	handle_request(out, cmd, path);
      }

      auto msg = out.str();

      if (sendto(sock, msg.data(), msg.size(), 0, static_cast<const sockaddr*>(addr), addr_len) != ssize_t(msg.size())){
	std::cerr << "sendto " << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()) << std::endl;
      }
    }

    bool init_http() {
      // no cleanup, no need
      evhttp* ev_web = evhttp_start(vm["bind_address"].as<std::string>().c_str(), vm["bind_port"].as<int>());
      if (!ev_web) {
	std::cerr << "evhttp_start " << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()) << std::endl;
	return false;
      }
      evhttp_set_gencb(ev_web, [] (evhttp_request *req, void *ptr) -> void {
	  static_cast<context*>(ptr)->handle_http_request(req);
	}, this );
      return true;
    }

    bool init_udp() {
      // no cleanup, no need
      auto sock = socket(AF_INET, SOCK_DGRAM, 0);
      if (sock < 0){
	std::cerr << "socket: " << EVUTIL_SOCKET_ERROR() << std::endl;
	return false;
      }
      if (evutil_make_socket_nonblocking(sock)) {
	std::cerr << "evutil_make_socket_nonblocking " << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()) << std::endl;
	return false;
      }
      if (evutil_make_listen_socket_reuseable(sock)) {
	std::cerr << "evutil_make_socket_reuseable " << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()) << std::endl;
	return false;
      }
    
      struct sockaddr_in addr;
      addr.sin_family = AF_INET;
      addr.sin_port = htons(vm["bind_port_udp"].as<int>());
      addr.sin_addr.s_addr = INADDR_ANY;

      if (bind(sock, (struct sockaddr *)&addr, sizeof(addr))) {
	std::cerr << "bind " << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()) << std::endl;
	return false;
      }

      event_set(&udp_event, sock, EV_READ | EV_PERSIST,  [] (evutil_socket_t sock, short what, void *ctx) -> void {
	  static_cast<context*>(ctx)->handle_udp_events(sock);
	}, this);
      event_add(&udp_event, NULL);
    
      return true;
    }
  };

  context* global_ctx;

  void finished_channel(int channel) {
    auto sequence = global_ctx->channel_to_sequence[channel];
    std::cout << time_millis() << " finished playing " << sequence << " on channel " << channel << std::endl;

    global_ctx->sequence_to_channel.erase(sequence);
    global_ctx->channel_to_sequence.erase(channel);
  }
}



int main(int argc, char*argv[]) {
  namespace po = boost::program_options;
  po::options_description description("Mix audio");
  description.add_options()
    ("help", "Display this help message")
    ("frequency", po::value<int>()->default_value(44100), "Frequency in Hz")
    ("channels", po::value<int>()->default_value(2), "Channels")
    ("chunksize", po::value<int>()->default_value(512), "Bytes sent to sound output each time, divide by frequency to find duration")
    ("sample-files", po::value<std::vector<std::string>>(), "OGG, WAV or MP3 sample files")
    ("allocate_sdl_channels", po::value<int>()->default_value(2048), "Number of SDL channels to mix together")
    ("bind_address", po::value<std::string>()->default_value("0.0.0.0"), "Address to listen on for HTTP")
    ("bind_port", po::value<int>()->default_value(13231), "Port to listen on for HTTP")
    ("bind_port_udp", po::value<int>()->default_value(13231), "Port to listen on for UDP")    
    ;
  po::variables_map vm;
  po::positional_options_description p;
  p.add("sample-files", -1);
  po::store(po::command_line_parser(argc, argv).options(description).positional(p).run(), vm);
  po::notify(vm);

  if(vm.count("help")){
    std::cerr << description;
    return 1;
  }

  int ret = SDL_Init(SDL_INIT_AUDIO);

  if (ret < 0) {
    std::cerr << "SDL_Init " << ret << " " << SDL_GetError() << std::endl;
    return 2;
  }


  ret = Mix_OpenAudio(vm["frequency"].as<int>(),AUDIO_S16SYS, vm["channels"].as<int>(), vm["chunksize"].as<int>());
  if (ret < 0) {
    std::cerr << "Mix_OpenAudio " << ret << " " << Mix_GetError() << std::endl;
    return 3;
  }

  Mix_AllocateChannels(vm["allocate_sdl_channels"].as<int>());


  context ctx(vm);
  global_ctx = &ctx; // as finished_channel needs a global
  Mix_ChannelFinished(finished_channel);

  
  if(vm.count("sample-files")){
    for (auto& file : vm["sample-files"].as<std::vector<std::string>>()) {
      std::cout << "Loading " << file << std::endl;
      auto chunk = Mix_LoadWAV(file.c_str());
      if (!chunk){
	std::cerr << "Could not load " << file << ": " << Mix_GetError() << std::endl;
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
  if (!ctx.init_http()){
    std::cerr <<"init_http" << std::endl;
  }
  
  if (event_dispatch() == -1) {
    std::cerr << "event_dispatch" << std::endl;
    return 7;
  }

  return 0;
}

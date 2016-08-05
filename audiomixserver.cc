#include <iostream>
#include <chrono>
#include <unordered_map>

#include "SDL.h"
#include "SDL_mixer.h"

#include "boost/program_options.hpp"

#include "evhttp.h"

struct context 
{
  std::unordered_map<std::string, Mix_Chunk *> chunks;
  std::chrono::time_point<std::chrono::system_clock> start = std::chrono::system_clock::now();	

  bool play(std::string const& name) {
    auto p = chunks.find(name);

    if (p == chunks.end()) {
      return false;
    }
    
    int ret = Mix_PlayChannel(-1, p->second, 0);
    if (ret < 0) {
      std::cerr << "Mix_PlayChannel " << ret << " " << p->first << " " << Mix_GetError() << std::endl;
    } else {
      std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();	
      std::cout << std::chrono::duration_cast<std::chrono::milliseconds>(now-start).count()  << " Playing " << p->first << " on channel " << ret << std::endl;
    }

    return true;
  }
  
  void handle_request(evhttp_request *req) {
    auto *out = evhttp_request_get_output_buffer(req);
    if (!out) {
      std::cerr << "evhttp_request_get_output_buffer" << std::endl;
      return;
    }
    auto uri = evhttp_request_get_evhttp_uri(req);
    auto path = evhttp_uri_get_path(uri);

    if (std::string("/") == path) {
      for (auto const& p : chunks) {
	evbuffer_add_printf(out, "<a href=\"%s\">%s</a>\n", p.first.c_str(), p.first.c_str());
      }
      evhttp_send_reply(req, HTTP_OK, "Pick a tune", out);
      return;
    }	
    
    if (play(path)) {
      evbuffer_add_printf(out, "<a href=\"%s\">Played</a>", path);
      evhttp_send_reply(req, HTTP_OK, "Mixing madly", out);
      return;
    }

    evbuffer_add_printf(out, "Not found. <a href=\"/\">Home beat.</a>\n");
    evhttp_send_reply(req, HTTP_NOTFOUND, "Not a tune", out);
  } 
};

int main(int argc, char*argv[]) {
  namespace po = boost::program_options;
  po::options_description description("Mix audio");
  description.add_options()
    ("help", "Display this help message")
    ("frequency", po::value<int>()->default_value(48000), "Frequency in Hz")
    ("channels", po::value<int>()->default_value(2), "Channels")
    ("chunksize", po::value<int>()->default_value(512), "Bytes sent to sound output each time, divide by frequency to find duration")
    ("sample-files", po::value<std::vector<std::string>>(), "OGG, WAV or MP3 sample files")
    ("allocate_sdl_channels", po::value<int>()->default_value(2048), "Number of SDL channels to mix together")
    ("bind_address", po::value<std::string>()->default_value("0.0.0.0"), "Address to listen on")
    ("bind_port", po::value<int>()->default_value(13231), "Port to listen on")
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

  context ctx;
  
  if(vm.count("sample-files")){
    for (auto& file : vm["sample-files"].as<std::vector<std::string>>()) {
        std::cout << "Loading " << file << std::endl;
	ctx.chunks[file] = Mix_LoadWAV(file.c_str());
    }
  }

  if (!event_init()) {
    std::cerr << "event_init" << std::endl;
    return 5;
  }


  std::unique_ptr<evhttp, decltype(&evhttp_free)> ev_web(evhttp_start(vm["bind_address"].as<std::string>().c_str(), vm["bind_port"].as<int>()), &evhttp_free);
  if (!ev_web) {
    std::cerr << "evhttp_start" << std::endl;
    return 6;
  }
  evhttp_set_gencb(ev_web.get(), [] (evhttp_request *req, void *ptr) -> void {
      static_cast<context*>(ptr)->handle_request(req);
    }, &ctx );
  if (event_dispatch() == -1) {
    std::cerr << "event_dispatch" << std::endl;
    return 7;
  }

  return 0;
}

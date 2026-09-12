#include "gpu.hpp"
#include "library.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <vector>
int main(int argc, char **argv) {
  try {
    if (argc != 4 && argc != 7)
      return 2;
    const int frames = argc == 7 ? std::stoi(argv[6]) : 90;
    if (frames < 1) throw std::runtime_error("frames must be positive");
    Gpu gpu(argc == 7 ? std::stoi(argv[4]) : 640,
            argc == 7 ? std::stoi(argv[5]) : 360, argv[2]);
    std::cerr << "loading model\n";
    gpu.load(load_mesh(argv[1], std::filesystem::path(argv[1]).parent_path()));
    std::cerr << "model loaded\n";
    std::ofstream out(argv[3], std::ios::binary);
    Camera camera;
    constexpr int warmup = 30;
    std::vector<double> render, convert, encode, frame;
    double started = 0;
    size_t bytes = 0;
    for (int n = 0; n < frames + warmup; n++) {
      if (n == warmup) started = now_ms();
      double frameStart = now_ms();
      int i = gpu.acquire();
      if (i < 0)
        throw std::runtime_error("slot leak");
      camera.yaw += .02f;
      FrameTiming t;
      t.frame = n + 1;
      if (n == 0)
        std::cerr << "render\n";
      gpu.render(i, camera, t);
      if (n == 0)
        std::cerr << "encode\n";
      const unsigned char *data;
      size_t length;
      int key;
      gpu.encode(i, n == 0, 2000000, &data, &length, &key);
      if (!length || (n == 0 && !key))
        throw std::runtime_error("invalid NVENC output");
      out.write((const char *)data, length);
      gpu.unlock(i);
      gpu.release(i);
      if (n >= warmup) {
        render.push_back(gpu.slots[i].timing.renderMs);
        convert.push_back(gpu.slots[i].timing.convertMs);
        encode.push_back(gpu.slots[i].timing.encodeEnd - gpu.slots[i].timing.encodeStart);
        frame.push_back(now_ms() - frameStart);
        bytes += length;
      }
      if (n == 0)
        std::cout << "frame " << n << " bytes " << length << " render "
                  << gpu.slots[i].timing.renderMs << "ms convert " << gpu.slots[i].timing.convertMs
                  << "ms encode " << gpu.slots[i].timing.encodeEnd - gpu.slots[i].timing.encodeStart
                  << "ms\n";
    }
    out.flush();
    if (!out) throw std::runtime_error("bitstream write failed");
    double elapsed = now_ms() - started;
    std::cout << "BENCH width=" << gpu.width << " height=" << gpu.height
              << " frames=" << frames << " elapsed_ms=" << elapsed
              << " fps=" << frames * 1000.0 / elapsed << " bytes=" << bytes << '\n';
    auto report = [](const char *name, std::vector<double> values) {
      std::sort(values.begin(), values.end());
      double sum = 0;
      for (double v : values) sum += v;
      std::cout << name << " mean_ms=" << sum / values.size()
                << " p50_ms=" << values[values.size() / 2]
                << " p95_ms=" << values[size_t((values.size() - 1) * .95)] << '\n';
    };
    report("render", render);
    report("convert", convert);
    report("encode", encode);
    report("frame", frame);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

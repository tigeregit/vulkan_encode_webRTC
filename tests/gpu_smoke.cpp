#include "gpu.hpp"
#include "library.hpp"
#include <fstream>
#include <iostream>
int main(int argc, char **argv) {
  try {
    if (argc != 4)
      return 2;
    Gpu gpu(640, 360, argv[2]);
    std::cerr << "loading model\n";
    gpu.load(load_mesh(argv[1], std::filesystem::path(argv[1]).parent_path()));
    std::cerr << "model loaded\n";
    std::ofstream out(argv[3], std::ios::binary);
    Camera camera;
    for (int n = 0; n < 90; n++) {
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
      if (n % 30 == 0)
        std::cout << "frame " << n << " bytes " << length << " render "
                  << gpu.slots[i].timing.renderMs << "ms convert " << gpu.slots[i].timing.convertMs
                  << "ms encode " << gpu.slots[i].timing.encodeEnd - gpu.slots[i].timing.encodeStart
                  << "ms\n";
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

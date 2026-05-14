#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>
#include <unistd.h>
#include <filesystem>
#include "App.h"
#include "knn.hpp"
#include "normalizer.hpp"
#include "simdjson.h"
#include "types.hpp"


namespace fs = std::filesystem;
using namespace simdjson;

std::unique_ptr<KNN> knn_engine;
Normalizer normalizer;

bool prepare_unix_socket(const std::string& path) {
    const fs::path socket_path(path);
    if (socket_path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(socket_path.parent_path(), ec);
        if (ec) {
            std::cerr << "fail to create socket directories: " << ec.message() << std::endl;
            return false;
        }
    }

    std::error_code ec;
    if (fs::exists(socket_path, ec) && !ec) {
        fs::remove(socket_path, ec);
        if (ec) {
            std::cerr << "fail to remove existing socket: " << ec.message() << std::endl;
            return false;
        }
    }

    return true;
}

int main(int argc, char *argv[]) {
  // Usage: <socket_path> <references.bin> <normalization.json> <mcc_risk.json>
  if (argc < 5) {
    std::cerr << "Uso: " << argv[0] << " <socket_path> <data.bin> <config.json> <stats.bin>" << std::endl;
    return 1;
  }

  const char* socket_path = argv[1];
  knn_engine = std::make_unique<KNN>(argv[2]);

  if (!normalizer.load_config(argv[4], argv[3])) return 1;

  static const char *CACHED_RESPONSES[6] = {
      R"({"approved": true, "fraud_score": 0.00})",
      R"({"approved": true, "fraud_score": 0.20})",
      R"({"approved": true, "fraud_score": 0.40})",
      R"({"approved": false, "fraud_score": 0.60})",
      R"({"approved": false, "fraud_score": 0.80})",
      R"({"approved": false, "fraud_score": 1.00})"};

  prepare_unix_socket(std::string(socket_path));

  uWS::App()
      .post("/fraud-score",
            [](auto *res, auto *req) {
              auto buffer = std::make_shared<std::string>();
              res->onData([res, buffer](std::string_view data, bool last) {
                buffer->append(data.data(), data.size());
                if (last) {
                  thread_local dom::parser parser;
                  float target_vec[14];

                  padded_string padded(*buffer);
                  dom::element doc;
                  if (!parser.parse(padded).get(doc)) {
                    try {
                      normalizer.normalize(doc, target_vec);
                      int count = knn_engine->get_fraud_count_exact(target_vec);
                      res
                         ->writeHeader("Content-Type", "application/json")
                        ->end(CACHED_RESPONSES[count]);
                    } catch (...) {
                      res->writeStatus("400")->end();
                    }
                  } else {
                    res->writeStatus("400")->end();
                  }
                }
              });
              res->onAborted([]() {});
            })
      .get("/ready", [](auto *res, auto *req) { res->end("OK"); })
      .listen([socket_path](auto *listen_socket) {
          if (listen_socket != nullptr) {
              std::cout << "Server listening on Unix Socket" << std::endl;
              chmod(std::string(socket_path).c_str(), 0777);
          } else {
              std::cerr << "Failed to listen" << std::endl;
              std::exit(1);
          }
      }, std::string(socket_path)).run();

  return 0;
}
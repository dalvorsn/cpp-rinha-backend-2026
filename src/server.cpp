#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

#include "App.h"
#include "knn.hpp"
#include "normalizer.hpp"
#include "simdjson.h"
#include "types.hpp"

using namespace simdjson;

std::unique_ptr<KNN> knn_engine;
Normalizer normalizer;

int main(int argc, char *argv[]) {
  if (argc < 4) return 1;

  knn_engine = std::make_unique<KNN>(argv[1]);

  if (!normalizer.load_config(argv[3], argv[2])) return 1;

  static const char *CACHED_RESPONSES[6] = {
      R"({"approved": true, "fraud_score": 0.00})",
      R"({"approved": true, "fraud_score": 0.20})",
      R"({"approved": true, "fraud_score": 0.40})",
      R"({"approved": false, "fraud_score": 0.60})",
      R"({"approved": false, "fraud_score": 0.80})",
      R"({"approved": false, "fraud_score": 1.00})"};

  uWS::App()
      .post("/fraud-score",
            [](auto *res, auto *req) {
              auto buffer = std::make_shared<std::string>();
              res->onData([res, buffer](std::string_view data, bool last) {
                buffer->append(data.data(), data.size());
                if (last) {
                  // thread_local: reuses parser and its internal buffers across
                  // requests
                  thread_local dom::parser parser;
                  float target_vec[14];

                  padded_string padded(*buffer);
                  dom::element doc;
                  if (!parser.parse(padded).get(doc)) {
                    try {
                      normalizer.normalize(doc, target_vec);
                      int count = knn_engine->get_fraud_count_exact(target_vec);
                      res->writeHeader("Content-Type", "application/json")
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
      .listen(9999,
              [](auto *listen_socket) {
                if (listen_socket)
                  std::cout << "Server listening on port 9999" << std::endl;
              })
      .run();

  return 0;
}

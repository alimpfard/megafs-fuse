#ifndef __MAID_CONTROLLER_H
#define __MAID_CONTROLLER_H

// clang-format off
#include <fuse.h>
#include <thread>
#include <vector>
#include "EventsHandler.h"
#include "metainfo.h"
// clang-format on

static const std::array<byte, SymmCipher::KEYLENGTH> cbytes {0};
class MegaFuse;
class MegaClient;
class MegaFuseModel;

class MAIDController {
public:
  std::vector<std::array<byte, SymmCipher::KEYLENGTH>> pwkeys;
  std::vector<MegaClient*> clients;
  std::vector<MegaFuseModel*> models;
  std::vector<std::mutex*> mutexes;
  std::vector<MetaInformation*> meta;

  EventsHandler eh;

  void event_loop(MegaFuse *that);
  bool login();
  MAIDController();
};

#endif /* end of include guard: __MAID_CONTROLLER_H */

#ifndef MAID_H
#define MAID_H
#include <thread>

class MegaClient;
class MegaFuseModel;

struct MAID {
  MegaFuseModel *model;
  MegaClient    *client;
  std::mutex    *engine_mutex;
};

#endif /* end of include guard: MAID_H */

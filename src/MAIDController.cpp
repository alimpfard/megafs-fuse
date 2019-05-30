// clang-format off
#include "megaclient.h"
#include "megafusemodel.h"
#include "Config.h"
#include <curl/curl.h>
#include "megacrypto.h"
#include <termios.h>
#include "megaposix.h"

#include <db_cxx.h>
#include "megabdb.h"
#include "MegaFuse.h"
#include "MaidController.h"
// clang-format on

void MAIDController::event_loop(MegaFuse *that) {
  printf("Start of event_loop\n");
  for (; that->running;) {
    for (int i = 0; i < clients.size(); i++) {
      auto *client = clients[i];
      auto *engine_mutex = mutexes[i];
      engine_mutex->lock();
      client->exec();
      engine_mutex->unlock();
      client->wait();
    }
  }
  printf("End of event_loop\n");

  if (that->needSerializeSymlink)
    that->serializeSymlink();
  if (that->needSerializeXattr)
    that->serializeXattr();

  that->running = false;
  that->event_loop_thread.join();
}

MAIDController::MAIDController()
    : pwkeys(std::move<decltype(pwkeys)>(
          {Config::getInstance()->userInfo.size(), cbytes})) {}

bool MAIDController::login() {
  int idxc = Config::getInstance()->userInfo.size();
  for (int idx = 0; idx < idxc; idx++) {
    std::string username =
        Config::getInstance()->userInfo[idx]->USERNAME.c_str();
    printf("Processing user %s (idx %d)...\n", username.c_str(), idx);
    std::string password =
        Config::getInstance()->userInfo[idx]->PASSWORD.c_str();
    auto *pwkey = pwkeys[idx].data();
    std::mutex *engine_mutex = new std::mutex;
    mutexes.push_back(engine_mutex);
    MegaFuseModel *model = new MegaFuseModel(eh, *engine_mutex);
    MegaApp *handler = model->getCallbacksHandler();
    MegaClient *client = new MegaClient(handler, new CurlHttpIO, new BdbAccess,
                                        Config::getInstance()->APPKEY.c_str());
    models.push_back(model);
    clients.push_back(client);
    model->client = client; // circular reference, ouch
    auto vmeta = new MetaInformation{
        Config::getInstance()->userInfo[idx]->LOADBALANCE_QUOTA,
        Config::getInstance()->userInfo[idx]->LOADBALANCE_QUOTA,
        Config::getInstance()->userInfo[idx]->LOADBALANCE_PRIORITY,
        idx
    };
    meta.push_back(vmeta);
    model->meta = vmeta;
    model->engine_mutex.lock();
    client->pw_key(password.c_str(), pwkey);
    client->login(username.c_str(), pwkey, 1);
    model->engine_mutex.unlock();

    {
      EventsListener el(eh, EventsHandler::LOGIN_RESULT);
      EventsListener eu(eh, EventsHandler::USERS_UPDATED);
      printf("waiting for login...\n");
      printf("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n");
      auto l_res = el.waitEvent();
      printf("done: %d\n", l_res.result);
      if (l_res.result < 0)
        return false;

      printf("waiting for user update...");
      eu.waitEvent();
      printf("done");
    }
  }
  return true;
}

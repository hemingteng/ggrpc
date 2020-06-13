#ifndef GGRPC_IMPL_CLIENT_MANAGER_H_INCLUDED
#define GGRPC_IMPL_CLIENT_MANAGER_H_INCLUDED

#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// gRPC
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/async_stream.h>
#include <grpcpp/support/async_unary_call.h>

// spdlog
#include <spdlog/spdlog.h>

#include "../alarm.h"
#include "../handler.h"
#include "client_reader.h"
#include "client_reader_writer.h"
#include "client_response_reader.h"
#include "client_writer.h"

namespace ggrpc {

class ClientManager;

class ClientManager {
  std::mutex mutex_;

  struct ThreadData {
    grpc::CompletionQueue cq;
    std::unique_ptr<std::thread> thread;
  };
  std::vector<ThreadData> threads_;

  uint32_t next_client_id_ = 0;
  bool shutdown_ = false;

  struct Holder {
    virtual ~Holder() {}
    virtual void Close() = 0;
    virtual bool Expired() = 0;
  };
  template <class W, class R>
  struct ResponseReaderHolder : Holder {
    std::weak_ptr<ClientResponseReader<W, R>> wp;
    ResponseReaderHolder(std::shared_ptr<ClientResponseReader<W, R>> p)
        : wp(p) {}
    void Close() override {
      auto sp = wp.lock();
      if (sp) {
        sp->Close();
      }
    }
    bool Expired() override { return wp.expired(); }
  };
  template <class W, class R>
  struct ReaderHolder : Holder {
    std::weak_ptr<ClientReader<W, R>> wp;
    ReaderHolder(std::shared_ptr<ClientReader<W, R>> p) : wp(p) {}
    void Close() override {
      auto sp = wp.lock();
      if (sp) {
        sp->Close();
      }
    }
    bool Expired() override { return wp.expired(); }
  };
  template <class W, class R>
  struct WriterHolder : Holder {
    std::weak_ptr<ClientWriter<W, R>> wp;
    WriterHolder(std::shared_ptr<ClientWriter<W, R>> p) : wp(p) {}
    void Close() override {
      auto sp = wp.lock();
      if (sp) {
        sp->Close();
      }
    }
    bool Expired() override { return wp.expired(); }
  };
  template <class W, class R>
  struct ReaderWriterHolder : Holder {
    std::weak_ptr<ClientReaderWriter<W, R>> wp;
    ReaderWriterHolder(std::shared_ptr<ClientReaderWriter<W, R>> p) : wp(p) {}
    void Close() override {
      auto sp = wp.lock();
      if (sp) {
        sp->Close();
      }
    }
    bool Expired() override { return wp.expired(); }
  };
  struct AlarmHolder : Holder {
    std::weak_ptr<Alarm> wp;
    AlarmHolder(std::shared_ptr<Alarm> p) : wp(p) {}
    void Close() override {
      auto sp = wp.lock();
      if (sp) {
        sp->Close();
      }
    }
    bool Expired() override { return wp.expired(); }
  };
  std::vector<std::unique_ptr<Holder>> holders_;

 public:
  ClientManager(int threads) : threads_(threads) {}

  ~ClientManager() { Shutdown(); }

  void Start() {
    for (auto& th : threads_) {
      th.thread.reset(new std::thread([& cq = th.cq]() { ThreadRun(&cq); }));
    }
  }

 private:
  static void ThreadRun(grpc::CompletionQueue* cq) {
    void* got_tag;
    bool ok = false;

    while (cq->Next(&got_tag, &ok)) {
      Handler* call = static_cast<Handler*>(got_tag);
      call->Proceed(ok);
    }
  }

  void Collect() {
    // expired な要素を削除する
    holders_.erase(std::remove_if(holders_.begin(), holders_.end(),
                                  [](const std::unique_ptr<Holder>& holder) {
                                    return holder->Expired();
                                  }),
                   holders_.end());
  }

 public:
  void Shutdown() {
    std::lock_guard<std::mutex> guard(mutex_);

    if (shutdown_) {
      return;
    }
    shutdown_ = true;

    SPDLOG_TRACE("ClientManager::Shutdown started");

    for (auto&& holder : holders_) {
      holder->Close();
    }
    holders_.clear();

    SPDLOG_TRACE("ClientManager::Shutdown all client closed");

    // まず通常のキューを Shutdown して、全てのスレッドが終了するのを待つ
    // コールバックの処理で無限ループしてるとかじゃない限りは終了するはず
    for (auto& th : threads_) {
      th.cq.Shutdown();
    }

    SPDLOG_TRACE("ClientManager::Shutdown cq shutdown completed");

    for (auto& th : threads_) {
      th.thread->join();
      th.thread = nullptr;
    }

    SPDLOG_TRACE("ClientManager::Shutdown finished");
  }

  template <class W, class R>
  std::shared_ptr<ClientResponseReader<W, R>> CreateResponseReader(
      typename ClientResponseReader<W, R>::RequestFunc request) {
    std::lock_guard<std::mutex> guard(mutex_);

    Collect();

    auto client_id = next_client_id_++;
    auto cq = &threads_[client_id % threads_.size()].cq;

    std::shared_ptr<ClientResponseReader<W, R>> p(
        new ClientResponseReader<W, R>(cq, std::move(request)),
        [](ClientResponseReader<W, R>* p) { p->Release(); });
    holders_.push_back(
        std::unique_ptr<Holder>(new ResponseReaderHolder<W, R>(p)));
    return p;
  }

  template <class W, class R>
  std::shared_ptr<ClientReader<W, R>> CreateReader(
      typename ClientReader<W, R>::ConnectFunc connect) {
    std::lock_guard<std::mutex> guard(mutex_);

    Collect();

    auto client_id = next_client_id_++;
    auto cq = &threads_[client_id % threads_.size()].cq;

    std::shared_ptr<ClientReader<W, R>> p(
        new ClientReader<W, R>(cq, std::move(connect)),
        [](ClientReader<W, R>* p) { p->Release(); });
    holders_.push_back(std::unique_ptr<Holder>(new ReaderHolder<W, R>(p)));
    return p;
  }

  template <class W, class R>
  std::shared_ptr<ClientWriter<W, R>> CreateWriter(
      typename ClientWriter<W, R>::ConnectFunc connect) {
    std::lock_guard<std::mutex> guard(mutex_);

    Collect();

    auto client_id = next_client_id_++;
    auto cq = &threads_[client_id % threads_.size()].cq;

    std::shared_ptr<ClientWriter<W, R>> p(
        new ClientWriter<W, R>(cq, std::move(connect)),
        [](ClientWriter<W, R>* p) { p->Release(); });
    holders_.push_back(std::unique_ptr<Holder>(new WriterHolder<W, R>(p)));
    return p;
  }

  template <class W, class R>
  std::shared_ptr<ClientReaderWriter<W, R>> CreateReaderWriter(
      typename ClientReaderWriter<W, R>::ConnectFunc connect) {
    std::lock_guard<std::mutex> guard(mutex_);

    Collect();

    auto client_id = next_client_id_++;
    auto cq = &threads_[client_id % threads_.size()].cq;

    std::shared_ptr<ClientReaderWriter<W, R>> p(
        new ClientReaderWriter<W, R>(cq, std::move(connect)),
        [](ClientReaderWriter<W, R>* p) { p->Release(); });
    holders_.push_back(
        std::unique_ptr<Holder>(new ReaderWriterHolder<W, R>(p)));
    return p;
  }

  std::shared_ptr<Alarm> CreateAlarm() {
    std::lock_guard<std::mutex> guard(mutex_);

    Collect();

    auto client_id = next_client_id_++;
    auto cq = &threads_[client_id % threads_.size()].cq;

    std::shared_ptr<Alarm> p(new Alarm(cq), [](Alarm* p) { p->Release(); });
    holders_.push_back(std::unique_ptr<Holder>(new AlarmHolder(p)));
    return p;
  }
};

}  // namespace ggrpc

#endif
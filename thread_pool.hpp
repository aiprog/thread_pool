#include <queue>
#include <functional>
#include <future>

namespace iret
{
  using namespace std;

  class thread_pool
  {
  public:
    thread_pool(size_t concurrency = thread::hardware_concurrency()) :
      concurrency_(!concurrency ? 1 : concurrency),
      done_(false)
    {
      try
      {
        resize(concurrency_);
      }
      catch (...)
      {
        notify_done();
        throw;
      }
    }

    ~thread_pool()
    {
      notify_done();
    }

    thread_pool(const thread_pool&) = delete;
    thread_pool &operator=(const thread_pool&) = delete;
    thread_pool(thread_pool&&) = delete;
    thread_pool &operator=(thread_pool&&) = delete;

    // thread-safe
    template<typename Func, typename... Args>
    auto submit(Func &&f, Args&&... args)
    {
      auto task = make_shared<packaged_task<invoke_result_t<Func, Args&...>()>>(
        bind(forward<Func>(f), forward<Args>(args)...));
      auto fut = task->get_future();

      if (index_ != -1)
      {
        auto &[mtx, que, quit] = info_;
        scoped_lock<mutex> lck(mtx);
        que.push([=] { (*task)(); });
      }
      else
      {
        scoped_lock<mutex> lck(mtx_);
        que_.push([=] { (*task)(); });
        cond_.notify_one();
      }

      return move(fut);
    }

    enum class run_state
    {
      ok = 1,
      empty,
      quit,
      done
    };

    // thread-safe
    run_state run_one(bool block)
    {
      if (index_ != -1)
      {
        auto state = run_private_work(info_, nullptr);
        if (state != run_state::empty)
          return state;
      }

      auto state = run_public_work(false);
      if (state != run_state::empty)
        return state;

      {
        unique_lock<mutex> lck(mtx_);

        auto beg = index_;
        auto len = concurrency_;

        if (beg == -1)
        {
          beg = hash<thread::id>()(this_thread::get_id()) % concurrency_;
          ++len;
        }

        for (size_t i = 1; i < len; ++i)
        {
          auto info = infos_[(beg + i) % concurrency_];
          if (!info)
            continue;

          auto state = run_private_work(*info, &lck);
          if (state == run_state::ok)
            return state;
        }
      }

      if (!block)
        return run_state::empty;

      return run_public_work(true);
    }

    // thread-safe
    size_t get_concurrency()
    {
      scoped_lock<mutex> lck(mtx_);
      return concurrency_;
    }

    // thread-safe
    void resize(size_t sz)
    {
      scoped_lock<mutex> lck(mtx_);

      for (auto i = sz; i < concurrency_; ++i)
      {
        auto &[mtx, que, quit] = *infos_[i];
        scoped_lock<mutex> lck(mtx);
        quit = true;
      }

      for (auto i = concurrency_; i < min(sz, workers_.size()); ++i)
      {
        if (!infos_[i])
        {
          workers_[i] = thread([=] { run(i); });
          return;
        }

        auto &[mtx, que, quit] = *infos_[i];
        scoped_lock<mutex> lck(mtx);

        quit = false;
      }

      for (auto i = workers_.size(); i < sz; ++i)
      {
        infos_.push_back(nullptr);
        workers_.push_back(thread([=] { run(i); }));
      }

      concurrency_ = sz;
      cond_.notify_all();
    }

  private:
    struct worker_info
    {
      worker_info() {}
      mutex mtx_;
      queue<function<void()>> que_;
      bool quit_ = false;
    };

    void run(size_t index)
    {
      index_ = index;

      {
        scoped_lock<mutex> lck(mtx_);
        infos_[index] = &info_;
      }

      while (1)
      {
        run_state state;
        while ((state = run_one(true)) == run_state::ok)
          ;

        if (state == run_state::done)
          return;

        if (state == run_state::quit)
        {
          auto &[mtx, que, quit] = info_;
          scoped_lock<mutex, mutex> lck(mtx_, mtx);

          if (!quit)
            continue;

          while (!que.empty())
          {
            que_.push(move(que.front()));
            que.pop();
          }

          workers_[index_].detach();
          infos_[index_] = nullptr;

          return;
        }
      }
    }

    run_state run_private_work(worker_info &info, unique_lock<mutex> *global_lck)
    {
      auto &[mtx, que, quit] = info;
      unique_lock<mutex> lck(info.mtx_);

      if (quit)
        return run_state::quit;

      if (que.empty())
        return run_state::empty;

      auto func = move(que.front());
      que.pop();
      lck.unlock();

      if (global_lck)
        global_lck->unlock();

      func();

      return run_state::ok;
    }

    run_state run_public_work(bool block)
    {
      unique_lock<mutex> lck(mtx_);
      cond_.wait(lck, [&]
      {
        if (!que_.empty() || done_ || !block)
          return true;

        if (index_ != -1)
        {
          scoped_lock<mutex> lck(info_.mtx_);
          if (info_.quit_)
            return true;
        }

        return false;
      });

      if (!que_.empty())
      {
        auto func = move(que_.front());
        que_.pop();
        lck.unlock();

        func();

        return run_state::ok;
      }

      if (done_)
        return run_state::done;

      if (!block)
        return run_state::empty;

      return run_state::quit;
    }

    void notify_done()
    {
      {
        scoped_lock<mutex> lck(mtx_);
        done_ = true;
        cond_.notify_all();
      }

      for_each(begin(workers_), end(workers_), [](thread &th) { if (th.joinable()) th.join(); });
    }

    mutex mtx_;
    condition_variable cond_;

    size_t concurrency_;
    bool done_;
    queue<function<void()>> que_;

    vector<thread> workers_;
    vector<worker_info*> infos_;

    inline thread_local static int index_ = -1;
    inline thread_local static worker_info info_;
  };
}
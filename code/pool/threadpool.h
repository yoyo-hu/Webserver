#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <functional>
//用c++11的线程库，可以改成linux下的线程库
class ThreadPool {
public:
    //默认八个线程
    explicit ThreadPool(size_t threadCount = 8): pool_(std::make_shared<Pool>()) {
            assert(threadCount > 0);
            //创建线程
            for(size_t i = 0; i < threadCount; i++) {
                //c++中创建线程的方式
                std::thread([pool = pool_] {
                    std::unique_lock<std::mutex> locker(pool->mtx);
                    while(true) {
                        if(!pool->tasks.empty()) {
                            //从任务队列中去任务
                            auto task = std::move(pool->tasks.front());
                            pool->tasks.pop();
                            locker.unlock();
                            //任务代码
                            task();
                            locker.lock();
                        } 
                        else if(pool->isClosed) break; //判断线程池是否关闭
                        else pool->cond.wait(locker); //但任务添加到队列的时候被唤醒
                    }
                }).detach();//设置线程分离
            }
    }

    ThreadPool() = default;

    ThreadPool(ThreadPool&&) = default;
    
    ~ThreadPool() {
        if(static_cast<bool>(pool_)) {
            {
                std::lock_guard<std::mutex> locker(pool_->mtx);
                pool_->isClosed = true;
            }
            pool_->cond.notify_all();
        }
    }

    //模板类
    template<class F>
    void AddTask(F&& task) {
        {
            std::lock_guard<std::mutex> locker(pool_->mtx);
            pool_->tasks.emplace(std::forward<F>(task));
        }
        pool_->cond.notify_one(); //唤醒一个线程
    }

private:
    //结构体
    struct Pool {
        std::mutex mtx; 
        std::condition_variable cond;
        bool isClosed;  //是否关闭
        std::queue<std::function<void()>> tasks;    //任务队列
    };
    std::shared_ptr<Pool> pool_;    //线程池
};


#endif //THREADPOOL_H
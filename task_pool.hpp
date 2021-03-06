/*************************************************
File name:  task_pool.hpp
Author:     caiyh
Version:
Date:
Description:    提供各类任务线程池基类,避免外界重复创建
*************************************************/
#pragma once
#include <mutex>
#include <vector>
#include <boost/noncopyable.hpp>
#include "safe_thread.hpp"
#include "task_queue.hpp"

namespace BTool
{
    /*************************************************
                   任务线程池基类
    *************************************************/
    class TaskPoolVirtual
    {
        enum {
            TP_MAX_THREAD = 2000,   // 最大线程数
        };

    public:
        TaskPoolVirtual() : m_cur_thread_ver(0) {
            m_atomic_switch.init();
        }
        virtual ~TaskPoolVirtual() {}

    public:
        // 是否已启动
        bool has_start() const {
            return m_atomic_switch.has_started();
        }

        // 是否已终止
        bool has_stop() const {
            return m_atomic_switch.has_stoped();
        }

        // 开启线程池
        // thread_num: 开启线程数,最大为STP_MAX_THREAD个线程,0表示系统CPU核数
        void start(size_t thread_num = std::thread::hardware_concurrency()) {
            if (!m_atomic_switch.start())
                return;

            std::lock_guard<std::mutex> lck(m_threads_mtx);
            create_thread(thread_num);
        }

        // 重置线程池个数,每缩容一个线程时会存在一个指针的内存冗余(线程资源会自动释放),执行stop函数或析构函数可消除该冗余
        // thread_num: 重置线程数,最大为STP_MAX_THREAD个线程,0表示系统CPU核数
        // 注意:必须开启线程池后方可生效
        void reset_thread_num(size_t thread_num = std::thread::hardware_concurrency()) {
            if (!m_atomic_switch.has_started())
                return;

            std::lock_guard<std::mutex> lck(m_threads_mtx);
            create_thread(thread_num);
        }

        // 终止线程池
        // 注意此处可能阻塞等待task的回调线程结束,故在task的回调线程中不可调用该函数
        // 完全停止后方可重新开启
        void stop() {
            if (!m_atomic_switch.stop())
                return;

            stop_inner();

            std::vector<SafeThread*> tmp_threads;
            {
                std::lock_guard<std::mutex> lck(m_threads_mtx);
                tmp_threads.swap(m_cur_thread);
            }
            for (auto& thread : tmp_threads) {
                delete thread;
                thread = nullptr;
            }
            tmp_threads.clear();
            m_atomic_switch.store_start_flag(false);
        }

    protected:
        // 内部执行stop操作
        virtual void stop_inner() = 0;
        // 内部执行pop任务操作,若无可pop内容时需阻塞
        virtual void pop_task_inner() = 0;

    private:
        // 创建线程
        void create_thread(size_t thread_num) {
            if (thread_num == 0) {
                thread_num = std::thread::hardware_concurrency();
            }
            ++m_cur_thread_ver;
            thread_num = thread_num < TP_MAX_THREAD ? thread_num : TP_MAX_THREAD;
            for (size_t i = 0; i < thread_num; i++) {
                m_cur_thread.push_back(new SafeThread(std::bind(&TaskPoolVirtual::thread_fun, this, m_cur_thread_ver.load())));
            }
        }

        // 线程池线程
        void thread_fun(size_t thread_ver) {
            while (true) {
                if (m_atomic_switch.has_stoped())
                    break;

                if (thread_ver < m_cur_thread_ver.load())
                    break;

                pop_task_inner();
            }
        }

    private:
        // 原子启停标志
        AtomicSwitch                m_atomic_switch;

        std::mutex                  m_threads_mtx;
        // 线程队列
        std::vector<SafeThread*>    m_cur_thread;
        // 当前设置线程版本号,每次重新设置线程数时,会递增该数值
        std::atomic<size_t>         m_cur_thread_ver;
    };

    /*************************************************
    Description:    提供并行有序执行的线程池
    1, 可同时添加多个任务;
    2, 所有任务有先后执行顺序,但可能会同时进行;
    4, 实时性:只要线程池线程有空闲的,那么提交任务后必须立即执行;尽可能提高线程的利用率。
    5. 提供可扩展或缩容线程池数量功能。
    *************************************************/
    class ParallelTaskPool
        : public TaskPoolVirtual
        , private boost::noncopyable
    {
    public:
        // 根据新增任务顺序并行有序执行的线程池
        // max_task_count: 最大任务缓存个数,超过该数量将产生阻塞;0则表示无限制
        ParallelTaskPool(size_t max_task_count = 0)
            : m_task_queue(max_task_count)
        {}

        virtual ~ParallelTaskPool() {
            clear();
            stop();
        }

        // 清空任务队列
        void clear() {
            m_task_queue.clear();
        }

        // 新增任务队列,超出最大任务数时存在阻塞
        // add_task([param1, param2=...]{...})
        // add_task(std::bind(&func, param1, param2))
        template<typename TFunction>
        bool add_task(TFunction&& func) {
            if (!has_start())
                return false;
            return m_task_queue.add_task(std::forward<TFunction>(func));
        }
        // 新增任务队列,超出最大任务数时存在阻塞
        // 特别注意!遇到char*/char[]等指针性质的临时指针,必须转换为string等实例对象,否则外界析构后,将指向野指针!!!!
//         template<typename TFunction, typename... Args>
//         bool add_task(TFunction&& func, Args&&... args) {
//             if (!has_start())
//                 return false;
//             return m_task_queue.add_task(std::forward<TFunction>(func), std::forward<Args>(args)...);
//         }

    protected:
        void stop_inner() override final {
            m_task_queue.stop();
        }

        virtual void pop_task_inner() override {
            m_task_queue.pop_task();
        }

    protected:
        TaskQueue               m_task_queue;
//         TupleTaskQueue          m_task_queue;
    };


    /*************************************************
    Description:    提供具有相同属性任务执行最新状态的线程池
    1, 每个属性都可以同时添加多个任务;
    2, 有很多的属性和很多的任务;
    3, 每个属性添加的任务必须有序串行执行,即在同一时刻不能有同时执行一个用户的两个任务;
    4, 实时性:只要线程池线程有空闲的,那么提交任务后必须立即执行;尽可能提高线程的利用率。
    5. 提供可扩展或缩容线程池数量功能。
    *************************************************/
    template<typename TPropType>
    class LastTaskPool
        : public TaskPoolVirtual
        , private boost::noncopyable
    {
    public:
        // 具有相同属性任务执行最新状态的线程池
        // max_task_count: 最大任务个数,超过该数量将产生阻塞;0则表示无限制
        LastTaskPool(size_t max_task_count = 0)
            : m_task_queue(max_task_count)
        {}

        ~LastTaskPool() {
            clear();
            stop();
        }

        void clear() {
            m_task_queue.clear();
        }

        // 新增任务队列,超出最大任务数时存在阻塞
        // add_task(prop, [param1, param2=...]{...})
        // add_task(prop, std::bind(&func, param1, param2))
        template<typename AsTPropType, typename TFunction>
        bool add_task(AsTPropType&& prop, TFunction&& func) {
            if (!has_start())
                return false;
            return m_task_queue.add_task(std::forward<AsTPropType>(prop), std::forward<TFunction>(func));
        }
        // 新增任务队列,超出最大任务数时存在阻塞
        // 特别注意!遇到char*/char[]等指针性质的临时指针,必须转换为string等实例对象,否则外界析构后,将指向野指针!!!!
//         template<typename AsTPropType, typename TFunction, typename... Args>
//         bool add_task(AsTPropType&& prop, TFunction&& func, Args&&... args) {
//             if (!has_start())
//                 return false;
//             return m_task_queue.add_task(std::forward<AsTPropType>(prop), std::forward<TFunction>(func), std::forward<Args>(args)...);
//         }

        template<typename AsTPropType>
        void remove_prop(AsTPropType&& prop) {
            m_task_queue.remove_prop(std::forward<AsTPropType>(prop));
        }

    protected:
        void stop_inner() override final {
            m_task_queue.stop();
        }

        void pop_task_inner() override {
            m_task_queue.pop_task();
        }

    private:
        // 待执行任务队列
        LastTaskQueue<TPropType>        m_task_queue;
//         LastTupleTaskQueue<TPropType>   m_task_queue;
    };

    /*************************************************
    Description:    提供具有相同属性任务串行有序执行的线程池
    1, 每个属性都可以同时添加多个任务;
    2, 有很多的属性和很多的任务;
    3, 每个属性添加的任务必须有序串行执行,即在同一时刻不能有同时执行一个用户的两个任务;
    4, 实时性:只要线程池线程有空闲的,那么提交任务后必须立即执行;尽可能提高线程的利用率。
    5. 提供可扩展或缩容线程池数量功能。
    *************************************************/
    template<typename TPropType>
    class SerialTaskPool
        : public TaskPoolVirtual
        , private boost::noncopyable
    {
    public:
        // 具有相同属性任务串行有序执行的线程池
        // max_task_count: 最大任务个数,超过该数量将产生阻塞;0则表示无限制
        SerialTaskPool(size_t max_task_count = 0)
            : m_task_queue(max_task_count)
        {}

        ~SerialTaskPool() {
            clear();
            stop();
        }

        // 清空任务队列
        void clear() {
            m_task_queue.clear();
        }

        // 新增任务队列,超出最大任务数时存在阻塞
        // add_task(prop, [param1, param2=...]{...})
        // add_task(prop, std::bind(&func, param1, param2))
        template<typename AsTPropType, typename TFunction>
        bool add_task(AsTPropType&& prop, TFunction&& func) {
            if (!has_start())
                return false;
            return m_task_queue.add_task(std::forward<AsTPropType>(prop), std::forward<TFunction>(func));
        }
        // 新增任务队列,超出最大任务数时存在阻塞
        // 特别注意!遇到char*/char[]等指针性质的临时指针,必须转换为string等实例对象,否则外界析构后,将指向野指针!!!!
//         template<typename AsTPropType, typename TFunction, typename... Args>
//         bool add_task(AsTPropType&& prop, TFunction&& func, Args&&... args) {
//             if (!has_start())
//                 return false;
//             return m_task_queue.add_task(std::forward<AsTPropType>(prop), std::forward<TFunction>(func), std::forward<Args>(args)...);
//         }

        template<typename AsTPropType>
        void remove_prop(AsTPropType&& prop) {
            m_task_queue.remove_prop(std::forward<AsTPropType>(prop));
        }

    protected:
        void stop_inner() override final {
            m_task_queue.stop();
        }

        void pop_task_inner() override {
            m_task_queue.pop_task();
        }

    private:
        // 待执行任务队列
        SerialTaskQueue<TPropType>          m_task_queue;
//         SerialTupleTaskQueue<TPropType>     m_task_queue;
    };

}
/*************************************************
File name:  task_queue.hpp
Author:     caiyh
Version:
Date:
Description:    �ṩ�����������,��������ظ�����

Note:  condition_variableʹ��ע��:�ڽ���waitʱ������
       1.ִ���ж�,Ϊtrue���˳�
       2.�ͷ�������(�ź���)����
       3.����notify,������
       Ȼ���ظ�1-3����,ֱ���ﵽ�����������˳�,ע���ʱ����Ϊ1������,��δ�ͷ���
*************************************************/
#pragma once
#include <queue>
#include <list>
#include <map>
#include <set>
#include <assert.h>
#include <condition_variable>
#include "rwmutex.hpp"
#include "atomic_switch.hpp"
#include "task_item.hpp"

namespace BTool
{
    template<typename TTaskType>
    class TaskQueueBase {
        // ��ֹ����
        TaskQueueBase(const TaskQueueBase&) = delete;
        TaskQueueBase& operator=(const TaskQueueBase&) = delete;

    public:
        TaskQueueBase(size_t max_task_count = 0)
            : m_max_task_count(max_task_count)
            , m_bstop(false)
        {}

        virtual ~TaskQueueBase() {
            clear();
            stop();
        }

        // �Ƴ�һ������ǵ�ǰִ����������,����Ϊ��ʱ��������
        void pop_task() {
            TTaskType pop_task(nullptr);
            {
                std::unique_lock<std::mutex> locker(this->m_mtx);
                this->m_cv_not_empty.wait(locker, [this] { return this->m_bstop.load() || this->not_empty(); });

                if (this->m_bstop.load())
                    return;

                pop_task = std::move(this->m_queue.front());
                this->m_queue.pop();
                this->m_cv_not_full.notify_one();
            }

            if (pop_task) {
                invoke(pop_task);
            }
        }

        void stop() {
            // �Ƿ�����ֹ�ж�
            bool target(false);
            if (!m_bstop.compare_exchange_strong(target, true)) {
                return;
            }

            m_cv_not_full.notify_all();
            m_cv_not_empty.notify_all();
        }

        void clear() {
            std::unique_lock<std::mutex> locker(m_mtx);
            std::queue<TTaskType> empty;
            m_queue.swap(empty);
            m_cv_not_full.notify_all();
            m_cv_not_empty.notify_all();
        }

        bool empty() const {
            std::unique_lock<std::mutex> locker(m_mtx);
            return !not_empty();
        }

        bool full() const {
            std::unique_lock<std::mutex> locker(m_mtx);
            return !not_full();
        }

        size_t size() const {
            std::unique_lock<std::mutex> locker(m_mtx);
            return m_queue.size();
        }

    protected:
        // �Ƿ���δ��״̬
        bool not_full() const {
            return m_max_task_count == 0 || m_queue.size() < m_max_task_count;
        }

        // �Ƿ��ڿ�״̬
        bool not_empty() const {
            return !m_queue.empty();
        }

        // ִ������
        virtual void invoke(TTaskType& task) = 0;

    protected:
        // �Ƿ�����ֹ��ʶ��
        std::atomic<bool>           m_bstop;
        // ���ݰ�ȫ��
        mutable std::mutex          m_mtx;

        // �ܴ�ִ���������,�������еĴ�ִ������
        std::queue<TTaskType>       m_queue;
        // ����������,��Ϊ0ʱ��ʾ������
        size_t                      m_max_task_count;

        // ��Ϊ�յ���������
        std::condition_variable     m_cv_not_empty;
        // û��������������
        std::condition_variable     m_cv_not_full;
    };

    /*************************************************
    Description:�ṩ���ں�����FIFO�������
    *************************************************/
    class TaskQueue : public TaskQueueBase<std::function<void()>>
    {
        typedef std::function<void()> TaskType;
    public:
        TaskQueue(size_t max_task_count = 0)
            : TaskQueueBase<TaskType>(max_task_count)
        {}

        virtual ~TaskQueue() {}

        template<typename AsTFunction>
        bool add_task(AsTFunction&& func) {
            std::unique_lock<std::mutex> locker(m_mtx);
            m_cv_not_full.wait(locker, [this] { return m_bstop.load() || not_full(); });

            if (m_bstop.load())
                return false;

            m_queue.push(std::forward<AsTFunction>(func));
            m_cv_not_empty.notify_one();
            return true;
        }

    protected:
        // ִ������
        void invoke(TaskType& task) override {
            task();
        }
    };

    /*************************************************
    Description:�ṩFIFO�������,�����ú���תΪԪ�����洢
    *************************************************/
    class TupleTaskQueue : public TaskQueueBase<std::shared_ptr<TaskVirtual>>
    {
        typedef std::shared_ptr<TaskVirtual>  TaskType;
    public:
        TupleTaskQueue(size_t max_task_count = 0)
            : TaskQueueBase<TaskType>(max_task_count)
        {}

        virtual ~TupleTaskQueue() {}

        template<typename TFunction, typename... Args>
        bool add_task(TFunction&& func, Args&&... args) {
            std::unique_lock<std::mutex> locker(m_mtx);
            m_cv_not_full.wait(locker, [this] { return m_bstop.load() || not_full(); });

            if (m_bstop.load())
                return false;

//             return add_task_tolist(std::make_shared<PackagedTask>(std::forward<TFunction>(func), std::forward<Args>(args)...));
            // �˴�TTuple���ɲ���std::forward_as_tuple(std::forward<Args>(args)...)
            // ��ʹagrs�к���const & ʱ,�ᵼ��tuple�д洢����Ϊconst &����,�Ӷ��ⲿ�ͷŶ�������ڲ�������Ч
            // ����std::make_shared<TTuple>��ᵼ�´���һ�ο���,��std::make_tuple����(const&/&&)
            typedef decltype(std::make_tuple(std::forward<Args>(args)...)) TTuple;
            return add_task_tolist(std::make_shared<TupleTask<TFunction, TTuple>>(std::forward<TFunction>(func), std::make_shared<TTuple>(std::forward_as_tuple(std::forward<Args>(args)...))));
        }

    protected:
        // ִ������
        void invoke(TaskType& task) override {
            task->invoke();
        }

      private:
        // ��������������
        bool add_task_tolist(TaskType&& new_task_item)
        {
            if (!new_task_item)
                return false;

            this->m_queue.push(std::forward<TaskType>(new_task_item));
            this->m_cv_not_empty.notify_one();
            return true;
        }
    };



    template<typename TPropType, typename TTaskType>
    class LastTaskQueueBase
    {
        // ��ֹ����
        LastTaskQueueBase(const LastTaskQueueBase&) = delete;
        LastTaskQueueBase& operator=(const LastTaskQueueBase&) = delete;

    public:
        // max_task_count: ����������,��������������������;0���ʾ������
        LastTaskQueueBase(size_t max_task_count = 0)
            : m_max_task_count(max_task_count)
            , m_bstop(false)
        {}
        virtual ~LastTaskQueueBase() {
            clear();
            stop();
        }

        // �Ƴ�һ������ǵ�ǰִ����������,����Ϊ��ʱ��������
        void pop_task() {
            TTaskType pop_task(nullptr);

            {
                std::unique_lock<std::mutex> locker(m_mtx);
                m_cv_not_empty.wait(locker, [this] { return m_bstop.load() || not_empty(); });

                if (m_bstop.load())
                    return;

                // �Ƿ����޿�pop����
                if (m_wait_props.empty())
                    return;

                auto& pop_type = m_wait_props.front();
                // ��ȡ����ָ��
                pop_task = std::move(m_wait_tasks[pop_type]);
                m_wait_tasks.erase(pop_type);
                m_wait_props.pop_front();

                m_cv_not_full.notify_one();
            }

            if (pop_task) {
                invoke(pop_task);
            }
        }

        // �Ƴ�����ָ����������,��ǰ����ִ�г���,���ܴ�������
        template<typename AsTPropType>
        void remove_prop(AsTPropType&& prop) {
            std::unique_lock<std::mutex> locker(m_mtx);
            m_wait_props.remove_if([prop](const TPropType& value)->bool {return (value == prop); });
            m_wait_tasks.erase(std::forward<AsTPropType>(prop));
            m_cv_not_full.notify_one();
        }

        void clear() {
            std::unique_lock<std::mutex> locker(m_mtx);
            m_wait_tasks.clear();
            m_wait_props.clear();
            m_cv_not_full.notify_all();
        }

        void stop() {
            // �Ƿ�����ֹ�ж�
            bool target(false);
            if (!m_bstop.compare_exchange_strong(target, true)) {
                return;
            }
            std::unique_lock<std::mutex> locker(m_mtx);
            m_cv_not_full.notify_all();
            m_cv_not_empty.notify_all();
        }

        bool empty() const {
            std::unique_lock<std::mutex> locker(m_mtx);
            return !not_empty();
        }

        bool full() const {
            std::unique_lock<std::mutex> locker(m_mtx);
            return !not_full();
        }

        size_t size() const {
            std::unique_lock<std::mutex> locker(m_mtx);
            return m_wait_props.size();
        }

    protected:
        // �Ƿ���δ��״̬
        bool not_full() const {
            return m_max_task_count == 0 || m_wait_props.size() < m_max_task_count;
        }

        // �Ƿ��ڷǿ�״̬
        bool not_empty() const {
            return !m_wait_props.empty();
        }

        // ִ������
        virtual void invoke(TTaskType& task) = 0;

    protected:
        // �Ƿ�����ֹ��ʶ��
        std::atomic<bool>                m_bstop;

        // ���ݰ�ȫ��
        mutable std::mutex               m_mtx;
        // �ܴ�ִ����������˳�����,�����ж�ִ�ж���˳��
        std::list<TPropType>             m_wait_props;
        // �ܴ�ִ������������Լ����Ӧ����,���������ʼ����m_wait_tasks����ͬ��
        std::map<TPropType, TTaskType>   m_wait_tasks;
        // ����������,��Ϊ0ʱ��ʾ������
        size_t                           m_max_task_count;

        // ��Ϊ�յ���������
        std::condition_variable          m_cv_not_empty;
        // û��������������
        std::condition_variable          m_cv_not_full;
    };

    /*************************************************
    Description:�ṩ�����Ի��ֵ�,����������״̬��FIFO�������,�����ú���תΪԪ�����洢
                ��ĳһ�������ڶ�����ʱ,ͬ���Ե�������������ʱ,ԭ����ᱻ����
    *************************************************/
    template<typename TPropType>
    class LastTaskQueue : public LastTaskQueueBase<TPropType, std::function<void()>>
    {
        typedef std::function<void()> TaskType;
    public:
        // max_task_count: ����������,��������������������;0���ʾ������
        LastTaskQueue(size_t max_task_count = 0)
            : LastTaskQueueBase<TPropType, TaskType>(max_task_count)
        {}
        ~LastTaskQueue() {}

        template<typename AsTPropType, typename AsTFunction>
        bool add_task(AsTPropType&& prop, AsTFunction&& func) {
            std::unique_lock<std::mutex> locker(this->m_mtx);
            this->m_cv_not_full.wait(locker, [this] { return this->m_bstop.load() || this->not_full(); });

            if (this->m_bstop.load())
                return false;

            auto iter = this->m_wait_tasks.find(prop);
            if (iter == this->m_wait_tasks.end())
                this->m_wait_props.push_back(prop);
            this->m_wait_tasks[std::forward<AsTPropType>(prop)] = std::forward<AsTFunction>(func);
            this->m_cv_not_empty.notify_one();
            return true;
        }

    protected:
        // ִ������
        void invoke(TaskType& task) override {
            task();
        }
    };

    /*************************************************
    Description:�ṩ�����Ի��ֵ�,����������״̬��FIFO�������,�����ú���תΪԪ�����洢
                ��ĳһ�������ڶ�����ʱ,ͬ���Ե�������������ʱ,ԭ����ᱻ����
    *************************************************/
    template<typename TPropType>
    class LastTupleTaskQueue : public LastTaskQueueBase<TPropType, std::shared_ptr<PropTaskVirtual<TPropType>>>
    {
        typedef std::shared_ptr<PropTaskVirtual<TPropType>> TaskType;

    public:
        // max_task_count: ����������,��������������������;0���ʾ������
        LastTupleTaskQueue(size_t max_task_count = 0)
            : LastTaskQueueBase<TPropType, TaskType>(max_task_count)
        {}
        ~LastTupleTaskQueue() {}

        template<typename AsTPropType, typename TFunction, typename... Args>
        bool add_task(AsTPropType&& prop, TFunction&& func, Args&&... args) {
            std::unique_lock<std::mutex> locker(this->m_mtx);
            this->m_cv_not_full.wait(locker, [this] { return this->m_bstop.load() || this->not_full(); });

            if (this->m_bstop.load())
                return false;

//             return add_task_tolist(std::make_shared<PropPackagedTask<TPropType>>(std::forward<AsTPropType>(prop), std::forward<TFunction>(func), std::forward<Args>(args)...));
            // �˴�TTuple���ɲ���std::forward_as_tuple(std::forward<Args>(args)...)
            // ��ʹagrs�к���const & ʱ,�ᵼ��tuple�д洢����Ϊconst &����,�Ӷ��ⲿ�ͷŶ�������ڲ�������Ч
            // ����std::make_shared<TTuple>��ᵼ�´���һ�ο���,��std::make_tuple����(const&/&&)
            typedef decltype(std::make_tuple(std::forward<Args>(args)...)) TTuple;
            return add_task_tolist(std::make_shared<PropTupleTask<TPropType, TFunction, TTuple>>(std::forward<AsTPropType>(prop), std::forward<TFunction>(func), std::make_shared<TTuple>(std::forward_as_tuple(std::forward<Args>(args)...))));
        }

    protected:
        // ִ������
        void invoke(TaskType& task) override {
            task->invoke();
        }

    private:
        // ��������������
        bool add_task_tolist(TaskType&& new_task_item)
        {
            if (!new_task_item)
                return false;

            auto& prop_type = new_task_item->get_prop_type();
            auto iter = this->m_wait_tasks.find(prop_type);
            if (iter == this->m_wait_tasks.end())
                this->m_wait_props.push_back(prop_type);
            this->m_wait_tasks[prop_type] = std::forward<TaskType>(new_task_item);

            this->m_cv_not_empty.notify_one();
            return true;
        }
    };

}
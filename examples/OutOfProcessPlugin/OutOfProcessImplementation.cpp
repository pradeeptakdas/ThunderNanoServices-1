/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#include <memory>

#include "Module.h"

#include "OutOfProcessPlugin.h"

namespace WPEFramework {
namespace Plugin {

    class OutOfProcessImplementation : public Core::Thread, public Exchange::IBrowser, public PluginHost::IStateControl {
    public:
        class Config : public Core::JSON::Container {
        private:
            Config(const Config&);
            Config& operator=(const Config&);

        public:
            Config()
                : Sleep(90)
                , Crash(false)
                , Destruct(1000)
            {
                Add(_T("sleep"), &Sleep);
                Add(_T("crash"), &Crash);
                Add(_T("destruct"), &Destruct);
            }
            ~Config()
            {
            }

        public:
            Core::JSON::DecUInt16 Sleep;
            Core::JSON::Boolean Crash;
            Core::JSON::DecUInt32 Destruct;
        };

        class Job : public Core::IDispatch {
        public:
            enum runtype {
                SHOW,
                HIDE,
                RESUMED,
                SUSPENDED
            };
            Job()
                : _parent(nullptr)
                , _type(SHOW)
            {
            }
            Job(OutOfProcessImplementation& parent, const runtype type)
                : _parent(&parent)
                , _type(type)
            {
            }
            Job(const Job& copy)
                : _parent(copy._parent)
                , _type(copy._type)
            {
            }
            ~Job()
            {
            }

            Job& operator=(const Job& RHS)
            {
                _parent = RHS._parent;
                _type = RHS._type;
                return (*this);
            }

        public:
            void Dispatch() override
            {

                switch (_type) {
                case SHOW:
                    _parent->Hidden(false);
                    _parent->_hidden = false;
                    break;
                case HIDE:
                    _parent->Hidden(true);
                    _parent->_hidden = true;
                    break;
                case RESUMED:
                    _parent->StateChange(PluginHost::IStateControl::RESUMED);
                    break;
                case SUSPENDED:
                    _parent->StateChange(PluginHost::IStateControl::SUSPENDED);
                    break;
                }
            }

        private:
            OutOfProcessImplementation* _parent;
            runtype _type;
        };

    private:
        OutOfProcessImplementation(const OutOfProcessImplementation&);
        OutOfProcessImplementation& operator=(const OutOfProcessImplementation&);

    public:
        OutOfProcessImplementation()
            : Core::Thread(0, _T("OutOfProcessImplementation"))
            , _reference()
            , _requestedURL()
            , _setURL()
            , _fps(0)
            , _hidden(false)
            , _executor(1, 0, 4)
        {
        }
        virtual ~OutOfProcessImplementation()
        {
            Block();

            if (Wait(Core::Thread::STOPPED | Core::Thread::BLOCKED, _config.Destruct.Value()) == false)
                TRACE_L1("Bailed out before the thread signalled completion. %d ms", _config.Destruct.Value());
        }

    public:
        virtual void SetURL(const string& URL)
        {
            _requestedURL = URL;

            TRACE(Trace::Information, (_T("New URL: %s"), URL.c_str()));

            TRACE_L1("Received a new URL: %s", URL.c_str());
            TRACE_L1("URL length: %u", static_cast<uint32_t>(URL.length()));

            Run();
        }

        virtual uint32_t Configure(PluginHost::IShell* service)
        {
            _dataPath = service->DataPath();
            _config.FromString(service->ConfigLine());
            _endTime = Core::Time::Now();

            if (_config.Sleep.Value() > 0) {
                TRACE_L1("Going to sleep for %d seconds.", _config.Sleep.Value());
                _endTime.Add(1000 * _config.Sleep.Value());
            }

            Run();
            return (Core::ERROR_NONE);
        }
        virtual string GetURL() const
        {
            string message;

            for (unsigned int teller = 0; teller < 120; teller++) {
                message += static_cast<char>('0' + (teller % 10));
            }
            return (message);
        }
        virtual bool IsVisible() const
        {
            return (!_hidden);
        }
        virtual uint32_t GetFPS() const
        {
            TRACE(Trace::Fatal, (_T("Fatal ingested: %d!!!"), _fps));
            return (++_fps);
        }
        virtual void Register(PluginHost::IStateControl::INotification* sink)
        {
            _adminLock.Lock();

            // Make sure a sink is not registered multiple times.
            ASSERT(std::find(_notificationClients.begin(), _notificationClients.end(), sink) == _notificationClients.end());

            _notificationClients.push_back(sink);
            sink->AddRef();

            TRACE_L1("IStateControl::INotification Registered in webkitimpl: %p", sink);
            _adminLock.Unlock();
        }

        virtual void Unregister(PluginHost::IStateControl::INotification* sink)
        {
            _adminLock.Lock();

            std::list<PluginHost::IStateControl::INotification*>::iterator index(std::find(_notificationClients.begin(), _notificationClients.end(), sink));

            // Make sure you do not unregister something you did not register !!!
            ASSERT(index != _notificationClients.end());

            if (index != _notificationClients.end()) {
                TRACE_L1("IStateControl::INotification Removing registered listener from browser %d", __LINE__);
                (*index)->Release();
                _notificationClients.erase(index);
            }

            _adminLock.Unlock();
        }
        virtual void Register(Exchange::IBrowser::INotification* sink)
        {
            _adminLock.Lock();

            // Make sure a sink is not registered multiple times.
            ASSERT(std::find(_browserClients.begin(), _browserClients.end(), sink) == _browserClients.end());

            _browserClients.push_back(sink);
            sink->AddRef();

            TRACE_L1("IBrowser::INotification Registered in webkitimpl: %p", sink);
            _adminLock.Unlock();
        }

        virtual void Unregister(Exchange::IBrowser::INotification* sink)
        {
            _adminLock.Lock();

            std::list<Exchange::IBrowser::INotification*>::iterator index(std::find(_browserClients.begin(), _browserClients.end(), sink));

            // Make sure you do not unregister something you did not register !!!
            ASSERT(index != _browserClients.end());

            if (index != _browserClients.end()) {
                TRACE_L1("IBrowser::INotification Removing registered listener from browser %d", __LINE__);
                (*index)->Release();
                _browserClients.erase(index);
            }

            _adminLock.Unlock();
        }

        virtual uint32_t Request(const PluginHost::IStateControl::command command)
        {

            uint32_t result(Core::ERROR_ILLEGAL_STATE);

            switch (command) {
            case PluginHost::IStateControl::SUSPEND:
                _executor.Submit(Core::ProxyType<Core::IDispatch>(Core::ProxyType<Job>::Create(*this, Job::SUSPENDED)), 20000);
                result = Core::ERROR_NONE;
                break;
            case PluginHost::IStateControl::RESUME:
                _executor.Submit(Core::ProxyType<Core::IDispatch>(Core::ProxyType<Job>::Create(*this, Job::RESUMED)), 20000);
                result = Core::ERROR_NONE;
                break;
            }

            return (result);
        }

        virtual PluginHost::IStateControl::state State() const
        {
            return (PluginHost::IStateControl::RESUMED);
        }

        virtual void Hide(const bool hidden)
        {

            if (hidden == true) {

                printf("Hide called. About to sleep for 2S.\n");
                _executor.Submit(Core::ProxyType<Core::IDispatch>(Core::ProxyType<Job>::Create(*this, Job::HIDE)), 20000);
                SleepMs(2000);
                printf("Hide completed.\n");
            } else {
                printf("Show called. About to sleep for 4S.\n");
                _executor.Submit(Core::ProxyType<Core::IDispatch>(Core::ProxyType<Job>::Create(*this, Job::SHOW)), 20000);
                SleepMs(4000);
                printf("Show completed.\n");
            }
        }

        virtual void Precondition(const bool)
        {
            printf("We are good to go !!!.\n");
        }
        virtual void Closure()
        {
            printf("Closure !!!!\n");
        }
        void StateChange(const PluginHost::IStateControl::state state)
        {
            _adminLock.Lock();

            std::list<PluginHost::IStateControl::INotification*>::iterator index(_notificationClients.begin());

            while (index != _notificationClients.end()) {
                TRACE_L1("State change from OutofProcessTest 0x%X", state);
                (*index)->StateChange(state);
                index++;
            }

            _adminLock.Unlock();
        }
        void Hidden(const bool hidden)
        {
            _adminLock.Lock();

            std::list<Exchange::IBrowser::INotification*>::iterator index(_browserClients.begin());

            while (index != _browserClients.end()) {
                TRACE_L1("State change from OutofProcessTest 0x%X", __LINE__);
                (*index)->Hidden(hidden);
                index++;
            }

            _adminLock.Unlock();
        }

        BEGIN_INTERFACE_MAP(OutOfProcessImplementation)
        INTERFACE_ENTRY(Exchange::IBrowser)
        INTERFACE_ENTRY(PluginHost::IStateControl)
        END_INTERFACE_MAP

    private:
        virtual uint32_t Worker()
        {
            if (Core::Time::Now() >= _endTime) {
                if (_config.Crash.Value() == true) {
                    TRACE_L1("Going to CRASH as requested %d.", 0);
                    abort();
                }

                exit(0);
            }

            if (_setURL != _requestedURL) {
                SleepMs(100);
                _adminLock.Lock();

                std::list<Exchange::IBrowser::INotification*>::iterator index(_browserClients.begin());

                _setURL = _requestedURL;

                while (index != _browserClients.end()) {
                    (*index)->URLChanged(_setURL);
                    index++;
                }

                _adminLock.Unlock();
            }

            // Just do nothing :-)
            Block();

            Core::Time now(Core::Time::Now());

            return (now > _endTime ? Core::infinite : static_cast<uint32_t>(1000));
        }

    private:
        mutable uint32_t _reference;
        Core::CriticalSection _adminLock;
        Config _config;
        string _requestedURL;
        string _setURL;
        string _dataPath;
        mutable uint32_t _fps;
        bool _hidden;
        Core::Time _endTime;
        std::list<PluginHost::IStateControl::INotification*> _notificationClients;
        std::list<Exchange::IBrowser::INotification*> _browserClients;
        Core::ThreadPool _executor;
    };

    SERVICE_REGISTRATION(OutOfProcessImplementation, 1, 0);

} // namespace Plugin

namespace OutOfProcessPlugin {

    class EXTERNAL MemoryObserverImpl : public Exchange::IMemory {
    private:
        MemoryObserverImpl();
        MemoryObserverImpl(const MemoryObserverImpl&);
        MemoryObserverImpl& operator=(const MemoryObserverImpl&);

    public:
        MemoryObserverImpl(const RPC::IRemoteConnection* connection)
            : _main(connection == nullptr ? Core::ProcessInfo().Id() : connection->RemoteId())
        {
        }
        ~MemoryObserverImpl()
        {
        }

    public:
        virtual uint64_t Resident() const
        {
            return _main.Resident();
        }
        virtual uint64_t Allocated() const
        {
            return _main.Allocated();
        }
        virtual uint64_t Shared() const
        {
            return _main.Shared();
        }
        virtual uint8_t Processes() const
        {
            return (IsOperational() ? 1 : 0);
        }
        virtual const bool IsOperational() const
        {
            return (_main.IsActive());
        }

        BEGIN_INTERFACE_MAP(MemoryObserverImpl)
        INTERFACE_ENTRY(Exchange::IMemory)
        END_INTERFACE_MAP

    private:
        Core::ProcessInfo _main;
    };

    Exchange::IMemory* MemoryObserver(const RPC::IRemoteConnection* connection)
    {
        ASSERT(connection != nullptr);
        Exchange::IMemory* result = Core::Service<MemoryObserverImpl>::Create<Exchange::IMemory>(connection);
        return (result);
    }
}
} // namespace WPEFramework::OutOfProcessPlugin

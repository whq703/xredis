#include "all.h"
#include "xEventLoop.h"
#include "xLog.h"

#ifdef __linux__
int createEventfd()
{
  int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (evtfd < 0)
  {
    assert(false);
  }
  return evtfd;
}
#endif

xEventLoop::xEventLoop()
:threadId(std::this_thread::get_id()),
#ifdef __linux__
 wakeupFd(createEventfd()),
 epoller(new xEpoll(this)),
 timerQueue(new xTimerQueue(this)),
 wakeupChannel(new xChannel(this,wakeupFd)),
#endif
#ifdef __APPLE__
 epoller(new xKqueue(this)),
 op(socketpair(AF_UNIX,SOCK_STREAM,0,wakeupFd)),
 //op(::pipe(wakeupFd)),
 timerQueue(new xTimerQueue(this)),
 wakeupChannel(new xChannel(this,wakeupFd[0])),
#endif
 currentActiveChannel(nullptr),
 running(false),
 eventHandling(false),
 callingPendingFunctors(false)
{
//#ifdef __APPLE__
//	socket.setFlag(wakeupFd[0],FD_CLOEXEC);
//	socket.setFlag(wakeupFd[1],FD_CLOEXEC);
//#endif
	wakeupChannel->setReadCallback(std::bind(&xEventLoop::handleRead, this));
	wakeupChannel->enableReading();
}


void xEventLoop::abortNotInLoopThread()
{
	assert(false);
}

xEventLoop::~xEventLoop()
{
	wakeupChannel->disableAll();
	wakeupChannel->remove();
#ifdef __linux__
	::close(wakeupFd);
#endif


#ifdef __APPLE__
	::close(wakeupFd[0]);
	::close(wakeupFd[1]);
#endif
}



void xEventLoop::updateChannel(xChannel* channel)
{
	assert(channel->ownerLoop() == this);
	assertInLoopThread();
	epoller->updateChannel(channel);
}


void xEventLoop::removeChannel(xChannel* channel)
{
	assert(channel->ownerLoop() == this);
	assertInLoopThread();
	if (eventHandling)
	{
		assert(currentActiveChannel == channel ||
		std::find(activeChannels.begin(), activeChannels.end(), channel) == activeChannels.end());
	}
	epoller->removeChannel(channel);
}

void xEventLoop::cancelAfter(xTimer * timer)
{
	timerQueue->cancelTimer(timer);
}

xTimer * xEventLoop::runAfter(double  when, void * data,bool repeat,xTimerCallback&& cb)
{
	return timerQueue->addTimer(when,data,repeat,std::move(cb));
}

bool xEventLoop::hasChannel(xChannel* channel)
{
	assert(channel->ownerLoop() == this);
	assertInLoopThread();
	return epoller->hasChannel(channel);
}

void  xEventLoop::handleRead()
{
	uint64_t one = 1;
#ifdef __linux__	
	ssize_t n = ::read(wakeupFd, &one, sizeof one);
#endif

#ifdef __APPLE__
	ssize_t n = ::read(wakeupFd[1], &one, sizeof one);
#endif
	if (n != sizeof one)
	{
		LOG_ERROR<<"xEventLoop::handleRead() reads error";
	}
}


void xEventLoop::quit()
{
	running = false;
	if (!isInLoopThread())
	{
		wakeup();
	}
}



void xEventLoop::wakeup()
{
  uint64_t one = 1;
#ifdef __linux__
  ssize_t n = ::write(wakeupFd, &one, sizeof one);
#endif

#ifdef __APPLE__
  ssize_t n = ::write(wakeupFd[0], &one, sizeof one);
#endif

  if (n != sizeof one)
  {
    LOG_ERROR<<"EventLoop::wakeup() wrties error";
  }

}


void xEventLoop::runInLoop(Functor&& cb)
{
	if (isInLoopThread())
	{
		cb();
	}
	else
	{

		queueInLoop(std::move(cb));
	}

}


void xEventLoop::queueInLoop(Functor&& cb)
{
	{
	 std::unique_lock<std::mutex> lk(mutex);
	 pendingFunctors.push_back(std::move(cb));

	}

	if (!isInLoopThread() || callingPendingFunctors)
	{
		wakeup();
	}
}

void xEventLoop::doPendingFunctors()
{
	std::vector<Functor> functors;
	callingPendingFunctors = true;

	{
		std::unique_lock<std::mutex> lk(mutex);
		functors.swap(pendingFunctors);
	}

	for (size_t i = 0; i < functors.size(); ++i)
	{
		functors[i]();
	}

	callingPendingFunctors = false;
}


void xEventLoop::run()
{
	running = true;
	while (running)
	{
		activeChannels.clear();
		epoller->epollWait(&activeChannels);
		eventHandling = true;
		for (auto it = activeChannels.begin();it != activeChannels.end(); ++it)
		{
		  currentActiveChannel = *it;
		  currentActiveChannel->handleEvent();
		}
		currentActiveChannel = nullptr;
		eventHandling = false;
		doPendingFunctors();
	}
}



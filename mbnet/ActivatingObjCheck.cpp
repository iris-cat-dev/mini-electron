
#include "mbnet/ActivatingObjCheck.h"
#include <cstdio>
#include <vector>

namespace net {

ActivatingObjCheck* ActivatingObjCheck::m_inst = nullptr;

ActivatingObjCheck::ActivatingObjCheck()
{
    m_newestId = 1;
    m_activatingObjs = new std::set<intptr_t>();
}

ActivatingObjCheck::~ActivatingObjCheck() = default;

ActivatingObjCheck* ActivatingObjCheck::inst()
{
    if (!m_inst)
        m_inst = new ActivatingObjCheck();
    return m_inst;
}

void ActivatingObjCheck::destroy()
{
    delete m_activatingObjs;
    m_activatingObjs = nullptr;

    if (m_inst)
        delete m_inst;
}

void ActivatingObjCheck::add(intptr_t loader)
{
    m_mutex.lock();
    m_activatingObjs->insert(loader);
    m_mutex.unlock();
}

void ActivatingObjCheck::remove(intptr_t loader)
{
    m_mutex.lock();
    m_activatingObjs->erase(loader);
    m_mutex.unlock();
}

bool ActivatingObjCheck::isActivating(intptr_t loader)
{
    m_mutex.lock();
    bool isActivating = m_activatingObjs->find(loader) != m_activatingObjs->end();
    m_mutex.unlock();
    return isActivating;
}

bool ActivatingObjCheck::isActivatingLocked(intptr_t loader)
{
    m_mutex.lock();
    return m_activatingObjs->find(loader) != m_activatingObjs->end();
}

int ActivatingObjCheck::genId()
{
    return m_newestId.fetch_add(1, std::memory_order_relaxed) + 1;
}

void ActivatingObjCheck::unlock()
{
    m_mutex.unlock();
}

void ActivatingObjCheck::testPrint()
{
#ifdef _DEBUG
    for (intptr_t object : *m_activatingObjs)
        std::fprintf(stderr, "ActivatingObjCheck::testPrint %p\n",
            reinterpret_cast<void*>(object));
#endif
}

void ActivatingObjCheck::doGarbageCollected(bool forceGC)
{
    if (!forceGC) {
        m_mutex.lock();
        int size = m_activatingObjs->size();
        m_mutex.unlock();
        if (size < 10)
            return;
    }

    m_mutex.lock();
    std::vector<intptr_t> activatingLoaders;
    activatingLoaders.resize(m_activatingObjs->size());

    std::set<intptr_t>::iterator setIt = m_activatingObjs->begin();
    std::set<intptr_t>::iterator end = m_activatingObjs->end();
    for (unsigned i = 0; setIt != end; ++setIt, ++i)
        activatingLoaders[i] = *setIt;

    m_mutex.unlock();

    //     std::vector<intptr_t>::iterator it = activatingLoaders.begin();
    //     for (; it != activatingLoaders.end(); ++it) {
    //         intptr_t loader = *it;
    //         double time = WTF::currentTimeMS();
    //         if (time - loader->startTime() > 10000 || forceGC) {
    //             loader->onTimeout();
    //         }
    //     }
}

void ActivatingObjCheck::shutdown()
{
    doGarbageCollected(true);
}

} // net
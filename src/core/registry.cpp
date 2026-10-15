#include "core/registry.h"

namespace sb {

bool Registry::add(std::unique_ptr<ITest> test)
{
    if (!test || test->info().id.isEmpty() || find(test->info().id))
        return false;
    m_tests.push_back(std::move(test));
    return true;
}

ITest *Registry::find(const QString &id) const
{
    const int i = indexOf(id);
    return i < 0 ? nullptr : m_tests[static_cast<size_t>(i)].get();
}

int Registry::indexOf(const QString &id) const
{
    for (size_t i = 0; i < m_tests.size(); ++i) {
        if (m_tests[i]->info().id == id)
            return static_cast<int>(i);
    }
    return -1;
}

QStringList Registry::ids() const
{
    QStringList out;
    for (const auto &t : m_tests)
        out.append(t->info().id);
    return out;
}

} // namespace sb

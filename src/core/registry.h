#pragma once

#include "core/itest.h"

#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace sb {

// The compiled-in tests (P4), in canonical order (P19): the order they were
// added, which for the built-in list is CPU, RAM, Storage, GPU, AI, NPU,
// Battery. Built-in tests are added by the generated registerBuiltinTests()
// (src/tests/builtin_tests.h); there are no self-registering static
// initialisers.
class Registry
{
public:
    // Refuses (returns false) a null test, an empty id or a duplicate id.
    bool add(std::unique_ptr<ITest> test);

    const std::vector<std::unique_ptr<ITest>> &tests() const { return m_tests; }
    ITest *find(const QString &id) const;
    // Canonical position, or -1.
    int indexOf(const QString &id) const;
    QStringList ids() const;
    int size() const { return static_cast<int>(m_tests.size()); }

private:
    std::vector<std::unique_ptr<ITest>> m_tests;
};

} // namespace sb

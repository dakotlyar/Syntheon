#pragma once

#include "core/enums.h"

#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

namespace sb {

// Option values: a flat map keyed by option key, saved per test result as
// JSON. Phase 9's CLI maps `--set storage.drive=…` onto the same keys.
using OptionValues = QVariantMap;

// P7: each test publishes a schema; one generic renderer builds its option tab.
enum class OptionKind {
    Bool,           // checkbox; value: bool
    MultiSelect,    // checkbox group, items can be disabled with a reason; value: QStringList
    SingleSelect,   // dropdown or segmented; value: QString (item value)
    DynamicSelect,  // dropdown filled by the plugin from the ProbeContext; value: QString
    Number,         // spin box; value: double
    Path,           // text + folder picker; value: QString
    Info,           // read-only label + value + pill; no value
};

enum class SelectPresentation { Dropdown, Segmented };

struct OptionItem {
    QString value;           // stable key
    QString label;
    bool enabled = true;
    QString disabledReason;  // "no Intel device"
    QString detail;          // secondary text, e.g. drive "path · type · free · fs"

    bool operator==(const OptionItem &) const = default;
};

struct OptionDef {
    QString key;  // stable forever
    QString label;
    OptionKind kind = OptionKind::Bool;
    QString help;
    QVariant defaultValue;

    // MultiSelect / SingleSelect / DynamicSelect
    QVector<OptionItem> items;
    SelectPresentation presentation = SelectPresentation::Dropdown;
    int minSelected = 0;  // MultiSelect only

    // Number
    double minimum = 0.0;
    double maximum = 0.0;
    double step = 1.0;
    QString unit;

    // Info
    QString infoValue;
    Tone infoTone = Tone::Neutral;

    const OptionItem *item(const QString &value) const;
    bool operator==(const OptionDef &) const = default;
};

struct OptionSchema {
    QVector<OptionDef> options;

    const OptionDef *find(const QString &key) const;
    // Every non-Info option's default value.
    OptionValues defaults() const;
    // `values` with any missing key filled from the defaults.
    OptionValues withDefaults(const OptionValues &values) const;

    bool operator==(const OptionSchema &) const = default;
};

struct ValidationError {
    QString key;  // empty for errors that aren't about one option
    QString message;

    bool operator==(const ValidationError &) const = default;
};

struct Validation {
    QVector<ValidationError> errors;

    bool ok() const { return errors.isEmpty(); }
    void add(const QString &key, const QString &message) { errors.append({key, message}); }
    static Validation success() { return {}; }
};

// Generic checks every schema gets: known keys, value types, item membership,
// disabled items, number ranges, minimum selection counts. Missing keys are
// fine (the default applies).
Validation validateOptions(const OptionSchema &schema, const OptionValues &values);

// Live pre-start check (Battery: "On AC: unplug", "100%", "Will lock at start").
struct Precondition {
    enum class State { Met, NotMet, WillApplyAtStart };

    QString id;
    QString label;
    State state = State::NotMet;
    QString detail;

    bool operator==(const Precondition &) const = default;
};

} // namespace sb

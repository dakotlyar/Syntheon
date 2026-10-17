// SPDX-License-Identifier: GPL-3.0-or-later
// A planted executable linked WITHOUT /DEPENDENTLOADFLAG, for the
// pe_loadflags_rejects_planted CTest: packaging/checks/pe-loadflags.py must
// fail on it (SynBench.exe can't be the negative control: it sets the flag
// too, in packaged builds, since Phase 1).
int main()
{
    return 0;
}

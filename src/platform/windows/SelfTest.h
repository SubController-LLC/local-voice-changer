#pragma once

namespace vvc::windows {

// Exercises code linked into the distributed GUI executable. A non-zero result identifies the
// failed stage and is observable by CI even though the executable uses the Windows GUI subsystem.
[[nodiscard]] int runExecutableSelfTest() noexcept;

} // namespace vvc::windows

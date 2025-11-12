## Code Style

Use the Google C++ style guide for C++.

You should store the pointer returned by the singleton GetInstance method. Avoid dereferencing GetInstance method directly using either -> or *. E.g., do not do ``hshm::Singleton<T>::GetInstance()->var_``. You should do ``auto *x = hshm::Singleton<T>::GetInstance(); x->var_;``.


NEVER use a null pool query. If you don't know, always use local.

Local QueueId should be named. NEVER use raw integers. This is the same for priorities. Please name them semantically.

All timing prints MUST include units of measurement in milliseconds (ms). Always print timing values in the order of milliseconds.

## Workflow
Use the incremental logic builder agent when making code changes.

Use the compiler subagent for making changes to cmakes and identifying places that need to be fixed in the code.

Always verify that code continue to compiles after making changes. Avoid commenting out code to fix compilation issues.

Whenever building unit tests, make sure to use the unit testing agent.

Whenever performing filesystem queries or executing programs, use the filesystem ops script agent.

NEVER DO MOCK CODE OR STUB CODE UNLESS SPECIFICALLY STATED OTHERWISE. ALWAYS IMPLEMENT REAL, WORKING CODE.

### Compilation Standards
- Always use the debug CMakePreset when compiling code in this repo
- Never hardcode paths in CMakeLists.txt files
- Use find_package() for all dependencies
- Follow ChiMod build patterns from MODULE_DEVELOPMENT_GUIDE.md
- All compilation warnings have been resolved as of the current state

## Code Quality Standards

### Compilation Standards
- All code must compile without warnings or errors
- Use appropriate variable types to avoid sign comparison warnings (e.g., `size_t` for container sizes)
- Mark unused variables with `(void)variable_name;` to suppress warnings when the variable is intentionally unused
- Follow strict type safety to prevent implicit conversions that generate warnings

## Device Configuration

### Directory Management
When configuring devices:
- Use `mkdir` to create each parent directory specified in the devices configuration during the configure phase
- Remove each directory during the clean phase to ensure proper cleanup
- This ensures device paths exist before use and are properly cleaned up after
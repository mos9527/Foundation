using namespace Foundation;
using namespace Core;


Mutex gLogImplMutex;
void Foundation_LogImpl(LogLevel level, const char* tag, std::string_view formatted)
{
    static auto init = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now() - init;
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    auto str = fmt::format("{}|+{:>5.5f}s|{} {}", level, ns / 1e9, tag, formatted);
    std::unique_lock lock(gLogImplMutex);
    fprintf(stderr, "%s\n", str.c_str());
}

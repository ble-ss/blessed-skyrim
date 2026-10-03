// skybench_log - the plugin log and events jsonl, for code outside skybench.cpp.
#pragma once

#include <string>

// one line in skybench.log.
void skybench_Log(const std::string & msg);

// one events jsonl line: {"t": <s>, "ev": "<ev>"<fields>}. fields is an
// already-comma-prefixed json fragment, or empty.
void skybench_LogEvent(const std::string & ev, const std::string & fields = std::string());

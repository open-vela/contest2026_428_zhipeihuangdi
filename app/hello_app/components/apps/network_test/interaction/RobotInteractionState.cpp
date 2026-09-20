#include "RobotInteractionState.hpp"

#include <cstdio>
#include <cstring>

namespace {
const char *skipSeparators(const char *text)
{
    if (!text) return "";
    while (*text != '\0') {
        if ((*text == ' ') || (*text == '\t') || (*text == ',') || (*text == '.') ||
            (*text == '!') || (*text == '?') || (*text == ':') || (*text == ';')) {
            ++text;
            continue;
        }
        if ((strncmp(text, "，", 3) == 0) || (strncmp(text, "。", 3) == 0) ||
            (strncmp(text, "！", 3) == 0) || (strncmp(text, "？", 3) == 0)) {
            text += 3;
            continue;
        }
        break;
    }
    return text;
}

}  // namespace

void RobotInteractionStateMachine::reset() { session_active_ = true; }

TranscriptAction RobotInteractionStateMachine::handleTranscript(
    const char *text, char *query, size_t query_size)
{
    if (query && query_size) query[0] = '\0';
    if (!text || text[0] == '\0') return TranscriptAction::Ignored;
    session_active_ = true;
    const char *content = text;
    content = skipSeparators(content);
    if (content[0] == '\0') return TranscriptAction::Ignored;
    if (query && query_size) snprintf(query, query_size, "%s", content);
    return TranscriptAction::Query;
}

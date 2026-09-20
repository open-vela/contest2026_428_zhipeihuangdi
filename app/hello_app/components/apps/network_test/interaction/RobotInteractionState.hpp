#pragma once

#include <cstddef>

enum class RobotInteractionState { AwaitingWake, Listening, Thinking, Speaking, Error };
enum class TranscriptAction { Ignored, Query };

class RobotInteractionStateMachine {
public:
    void reset();
    TranscriptAction handleTranscript(const char *text, char *query, size_t query_size);
    bool sessionActive() const { return session_active_; }

private:
    bool session_active_ = false;
};

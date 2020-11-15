#ifndef _OBS_MESSAGES_EVENTS_H_
#define _OBS_MESSAGES_EVENTS_H_

#define FOREACH_EVENT(EVENT)             \
    EVENT(Scenes)                        \
    EVENT(SwitchScenes)                  \
    EVENT(ScenesChanged)                 \
    EVENT(SceneCollectionChanged)        \
    EVENT(SceneCollectionListChanged)    \
    EVENT(SwitchTransition)              \
    EVENT(TransitionListChanged)         \
    EVENT(TransitionDurationChanged)     \
    EVENT(TransitionBegin)               \
    EVENT(TransitionEnd)                 \
    EVENT(TransitionVideoEnd)            \
    EVENT(ProfileChanged)                \
    EVENT(ProfileListChanged)            \
    EVENT(StreamStarting)                \
    EVENT(StreamStarted)                 \
    EVENT(StreamStopping)                \
    EVENT(StreamStopped)                 \
    EVENT(StreamStatus)                  \
    EVENT(RecordingStarting)             \
    EVENT(RecordingStarted)              \
    EVENT(RecordingStopping)             \
    EVENT(RecordingStopped)              \
    EVENT(RecordingPaused)               \
    EVENT(RecordingResumed)              \
    EVENT(ReplayStarting)                \
    EVENT(ReplayStarted)                 \
    EVENT(ReplayStopping)                \
    EVENT(ReplayStopped)                 \
    EVENT(Exiting)                       \
    EVENT(Heartbeat)                     \
    EVENT(BroadcastCustomMessage)        \
    EVENT(SourceCreated)                 \
    EVENT(SourceDestroyed)               \
    EVENT(SourceVolumeChanged)           \
    EVENT(SourceMuteStateChanged)        \
    EVENT(SourceAudioDeactivated)        \
    EVENT(SourceAudioActivated)          \
    EVENT(SourceAudioSyncOffsetChanged)  \
    EVENT(SourceAudioMixersChanged)      \
    EVENT(SourceRenamed)                 \
    EVENT(SourceFilterAdded)             \
    EVENT(SourceFilterRemoved)           \
    EVENT(SourceFilterVisibilityChanged) \
    EVENT(SourceFiltersReordered)        \
    EVENT(MediaPlaying)                  \
    EVENT(MediaPaused)                   \
    EVENT(MediaRestarted)                \
    EVENT(MediaStopped)                  \
    EVENT(MediaNext)                     \
    EVENT(MediaPrevious)                 \
    EVENT(MediaStarted)                  \
    EVENT(MediaEnded)                    \
    EVENT(SourceOrderChanged)            \
    EVENT(SceneItemAdded)                \
    EVENT(SceneItemRemoved)              \
    EVENT(SceneItemVisibilityChanged)    \
    EVENT(SceneItemLockChanged)          \
    EVENT(SceneItemTransformChanged)     \
    EVENT(SceneItemSelected)             \
    EVENT(SceneItemDeselected)           \
    EVENT(PreviewSceneChanged)           \
    EVENT(StudioModeSwitched)
#endif
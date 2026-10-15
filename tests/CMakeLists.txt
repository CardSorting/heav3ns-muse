include(CTest)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)

set(LMMS_TESTS
	src/core/ArrayVectorTest.cpp
	src/core/AudioBufferTest.cpp
	src/core/AutomatableModelTest.cpp
	src/core/NativeParameterConversionTest.cpp
	src/core/StudioParameterReadbackTest.cpp
	src/core/StudioCapabilityCancellationTest.cpp
	src/core/TimeSignatureSafetyTest.cpp
	src/core/JournalIdAllocatorTest.cpp
	src/core/MathTest.cpp
	src/core/ProjectVersionTest.cpp
	src/core/RelativePathsTest.cpp
	src/core/TimelineTest.cpp
	src/core/StudioRuntimeTest.cpp
	src/core/SampleClipResourceTest.cpp
	src/core/StudioProjectResourceTest.cpp
	src/core/StudioResourceNameTest.cpp
	src/core/FileSearchTest.cpp
	src/core/StudioAssetsTest.cpp
	src/core/LfoWaveResourceTest.cpp
	src/core/EnvelopeWaveResourceTest.cpp
	src/core/StudioCapabilitySchemaTest.cpp
	src/core/StudioCapabilityPreflightTest.cpp
	src/core/StudioClipEditTest.cpp
	src/core/StudioTrackSoloTest.cpp
	src/core/StudioTrackLifecycleTest.cpp
	src/core/PatternQueryTest.cpp
	src/core/StudioAudioIdentityTest.cpp
	src/core/StudioExportSettingsTest.cpp
	src/core/StudioStemTest.cpp
	src/core/TrackRenderScopeLifecycleTest.cpp
	src/core/RenderManagerPreparationTest.cpp
	src/core/RenderManagerLifecycleTest.cpp
	src/core/RenderManagerConstructionTest.cpp
	src/core/RenderManagerPublicationTest.cpp
	src/core/NativeAudioEncoderTest.cpp
	src/core/ProjectRendererFailureTest.cpp
	src/core/StudioRenderOwnershipTest.cpp
	src/core/StudioRenderCancellationTest.cpp
	src/core/SongExportTransitionTest.cpp
	src/core/StudioRecoveryTest.cpp
	src/core/StudioSessionReentryTest.cpp
	src/core/StudioContextTest.cpp
	src/core/StudioToolResultTest.cpp
	src/core/StudioReceiptTest.cpp
	src/core/StudioTranscriptTest.cpp
	src/core/StudioShutdownTest.cpp
	src/core/StudioStorageTest.cpp
	src/core/StudioArchivePathTest.cpp
	src/core/StudioResourcesTest.cpp
	src/core/StudioResourceBudgetTest.cpp
	src/core/StudioBackendTest.cpp
	src/core/StudioBackendSelectionTest.cpp
	src/core/AudioEngineFaultTest.cpp
	src/core/DspCacheRecoveryTest.cpp
	src/core/LinkedParameterTest.cpp
	src/core/LinkedProjectPersistenceTest.cpp
	src/core/LinkedTrackPersistenceTest.cpp
	src/core/LinkedClipPersistenceTest.cpp
	src/core/LinkedInlinePersistenceTest.cpp
	src/core/NativeClipTransferTest.cpp
	src/core/NativeNoteTransferTest.cpp
	src/core/StudioClipSelectionCopyTest.cpp
	src/core/StudioNoteSelectionCopyTest.cpp
	src/core/StudioNoteSelectionShiftTest.cpp
	src/core/StudioNoteSelectionQuantizeTest.cpp
	src/core/StudioNoteSelectionReverseTest.cpp
	src/core/StudioNoteSelectionLengthTest.cpp
	src/core/StudioNoteSelectionGlueTest.cpp
	src/core/StudioNoteSelectionRemoveTest.cpp
	src/core/StudioNoteSelectionEditTest.cpp
	src/core/StudioNoteBatchAddTest.cpp
	src/core/StudioNoteSelectionSplitTest.cpp
	src/core/StudioNoteSelectionKnifeTest.cpp
	src/core/StudioStepCloneTest.cpp
	src/core/StudioStepResizeTest.cpp
	src/core/StudioStepSetTest.cpp
	src/core/StudioParameterLinkTest.cpp
	src/core/LinkedValueOwnershipTest.cpp
	src/core/ModelSetterOwnershipTest.cpp
	src/core/ModelValueNotificationTest.cpp
	src/core/NativeValueNotificationTest.cpp
	src/core/LinkedValueJournalTest.cpp
	src/core/ControlLinkJournalTest.cpp
	src/core/NativeGraphTransitionTest.cpp
	src/core/AudioBusOutputTest.cpp
	src/core/AudioEngineWorkerTest.cpp
	src/core/MixerProcessingTest.cpp
	src/core/MixerSoloTest.cpp
	src/core/MixerStructureTest.cpp
	src/core/MixerLifecycleTest.cpp
	src/core/EffectChainTest.cpp
	src/core/EffectChainLifecycleTest.cpp
	src/core/StudioEffectControlsTest.cpp
	src/core/StudioParameterPagingTest.cpp
	src/core/StudioProjectInspectionTest.cpp
	src/core/StudioWarningInspectionTest.cpp
	src/core/StudioMixInspectionTest.cpp
	src/core/StudioAgentRestoreTest.cpp
	src/core/StudioSelectionTest.cpp
	src/core/StudioReplanningTest.cpp
	src/core/StudioHistoryIndexTest.cpp
	src/core/GraphModelTest.cpp
	src/core/StudioGraphTest.cpp
	src/core/StudioExpressionTest.cpp
	src/core/StudioPluginProgramTest.cpp
	src/core/StudioPluginPresetTest.cpp
	src/gui/MixerViewLifecycleTest.cpp
	src/core/AudioDeviceTransitionTest.cpp
	src/core/StudioRecordingTest.cpp
	src/core/StudioMidiRecordingTest.cpp
	src/core/StudioAutomationRecordingTest.cpp
	src/core/AutomationTakeCommitTest.cpp
	src/core/AutomationTakeReservationTest.cpp
	src/core/StudioCurveEditTest.cpp
	src/core/StudioCurveEraseTest.cpp
	src/core/StudioAutomationBindingTest.cpp
	src/core/StudioControllerDisconnectTest.cpp
	src/core/StudioMidiAssignmentTest.cpp
	src/core/StudioControllerAssignmentTest.cpp
	src/core/StudioControllerLifecycleTest.cpp
	src/core/SongControllerNotificationTest.cpp
	src/core/StudioMidiControllerTest.cpp
	src/core/StudioMidiLearnTest.cpp
	src/core/StudioTrackMoveTest.cpp
	src/core/StudioPatternTest.cpp
	src/core/StudioClipCopyTest.cpp
	src/core/StudioTrackCloneTest.cpp
	src/core/MidiPortTest.cpp
	src/core/InstrumentMidiCCTest.cpp
	src/core/InstrumentMidiEventTest.cpp
	src/core/InstrumentLoadLifecycleTest.cpp
	src/core/InstrumentPresetLifecycleTest.cpp
	src/core/InstrumentPresetSaveTest.cpp
	src/core/ProjectSaveLifecycleTest.cpp
	src/core/ProjectLoadLifecycleTest.cpp
	src/core/ProjectCreationLifecycleTest.cpp
	src/core/ImportFilterLifecycleTest.cpp
	src/core/MidiInputRenderTest.cpp
	src/core/SongExportRecoveryTest.cpp
	src/core/SongAutomationOwnershipTest.cpp
	src/core/SongAutomationInterruptionTest.cpp
	src/core/ControllerConnectionRestoreTest.cpp
	src/core/LfoCatchUpTest.cpp
	src/core/LfoRateNotificationTest.cpp
	src/core/TempoNotificationRecoveryTest.cpp
	src/core/ControllerStateTest.cpp
	src/core/ControllerSettingsLifecycleTest.cpp
	src/core/MidiPortLifecycleTest.cpp
	src/core/MidiClientTransferTest.cpp
	src/core/MidiClientRetirementTest.cpp
	src/core/RawMidiOutputTest.cpp
	src/core/StudioProtocolOutputTest.cpp
	src/core/StudioAssetStoreTest.cpp
	src/core/FileResourceSnapshotTest.cpp
	src/core/FileResourceIOTest.cpp
	src/core/DrumSynthTest.cpp
	src/core/SampleBufferTest.cpp
	src/core/SampleRecordingBufferTest.cpp
	src/core/StudioInferenceTest.cpp
	src/core/RemotePluginTest.cpp
	src/tracks/AutomationTrackTest.cpp
	src/plugins/GigPcmTest.cpp
	src/plugins/VstProgramsTest.cpp
	src/plugins/VstParameterWireTest.cpp
	src/plugins/VstParameterTransferTest.cpp
	src/plugins/VstPeFileTest.cpp
	src/plugins/VstMidiBufferTest.cpp
	src/plugins/VstPresetTest.cpp
	src/plugins/VstPresetStorageTest.cpp
)

if(TARGET midiexport)
	list(APPEND LMMS_TESTS src/plugins/MidiExportTest.cpp)
endif()
if(TARGET midiimport)
	list(APPEND LMMS_TESTS src/plugins/MidiImportTest.cpp)
endif()
if(TARGET hydrogenimport AND TARGET audiofileprocessor)
	list(APPEND LMMS_TESTS src/plugins/HydrogenImportTest.cpp)
endif()
if(TARGET audiofileprocessor)
	list(APPEND LMMS_TESTS src/plugins/AudioFileProcessorLifecycleTest.cpp)
endif()
if(TARGET tripleoscillator)
	list(APPEND LMMS_TESTS src/plugins/TripleOscillatorResourceTest.cpp)
endif()
if(LMMS_HAVE_FLUIDSYNTH)
	list(APPEND LMMS_TESTS src/plugins/Sf2SynthStateTest.cpp)
endif()
if(TARGET gigplayer)
	list(APPEND LMMS_TESTS src/plugins/GigResourceLifecycleTest.cpp src/plugins/GigPendingBankTest.cpp src/plugins/GigPatchDialogTest.cpp)
endif()
if(TARGET sf2player)
	list(APPEND LMMS_TESTS src/plugins/Sf2ResourceLifecycleTest.cpp src/plugins/Sf2PendingBankTest.cpp src/plugins/Sf2PatchDialogTest.cpp src/plugins/Sf2RenderAuditionTest.cpp src/plugins/Sf2RateFallbackTest.cpp)
endif()
if(TARGET exprtk)
	list(APPEND LMMS_TESTS src/plugins/XpressiveExpressionTest.cpp)
endif()
if(TARGET vstbase)
	list(APPEND LMMS_TESTS src/plugins/VstParameterModelsTest.cpp)
endif()
# These tests load real native test modules through the production worker.
# Linux currently identifies .so VSTs as 64-bit; Windows uses PE machine type.
if(TARGET vstbase AND ((LMMS_BUILD_LINUX AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND TARGET NativeLinuxRemoteVstPlugin64)
    OR (LMMS_BUILD_WIN32 AND ((CMAKE_SIZEOF_VOID_P EQUAL 8 AND TARGET RemoteVstPlugin64)
        OR (CMAKE_SIZEOF_VOID_P EQUAL 4 AND TARGET RemoteVstPlugin32)))))
	list(APPEND LMMS_TESTS src/plugins/VstNativeWorkerTest.cpp)
endif()
if(TARGET carlabase)
	list(APPEND LMMS_TESTS src/plugins/CarlaHostTest.cpp src/plugins/CarlaPluginDiscoveryTest.cpp src/plugins/CarlaPluginPatchbayTest.cpp)
endif()
if(LMMS_HAVE_PORTAUDIO)
	list(APPEND LMMS_TESTS src/core/PortAudioCallbackTest.cpp)
endif()
if(LMMS_HAVE_PULSEAUDIO)
	list(APPEND LMMS_TESTS src/core/PulseAudioTest.cpp)
endif()
if(LMMS_HAVE_ALSA)
	list(APPEND LMMS_TESTS src/core/MidiAlsaSeqTest.cpp src/core/AudioAlsaTest.cpp)
endif()
if(LMMS_HAVE_ALSA OR LMMS_HAVE_SNDIO)
	list(APPEND LMMS_TESTS src/core/RawMidiIoTest.cpp)
endif()
if(LMMS_HAVE_WINMM)
	list(APPEND LMMS_TESTS src/core/WinMmMidiIoTest.cpp)
endif()
if(LMMS_HAVE_SNDIO)
	list(APPEND LMMS_TESTS src/core/AudioSndioTest.cpp)
endif()
if(LMMS_HAVE_JACK)
	list(APPEND LMMS_TESTS src/core/AudioJackTest.cpp)
	list(APPEND LMMS_TESTS src/core/JackLifetimeTest.cpp)
endif()
if(LMMS_BUILD_APPLE)
	list(APPEND LMMS_TESTS src/core/MidiAppleTest.cpp)
endif()

foreach(LMMS_TEST_SRC IN LISTS LMMS_TESTS)
	# TODO CMake 3.20: Use cmake_path
	get_filename_component(LMMS_TEST_NAME ${LMMS_TEST_SRC} NAME_WE)

	add_executable(${LMMS_TEST_NAME} ${LMMS_TEST_SRC})
	add_test(NAME ${LMMS_TEST_NAME} COMMAND ${LMMS_TEST_NAME})

	# TODO CMake 3.12: Propagate usage requirements by linking to lmmsobjs
	target_include_directories(${LMMS_TEST_NAME} PRIVATE $<TARGET_PROPERTY:lmmsobjs,INCLUDE_DIRECTORIES>)

	target_static_libraries("${LMMS_TEST_NAME}" PRIVATE lmmsobjs)
	target_link_libraries(${LMMS_TEST_NAME} PRIVATE
		${QT_LIBRARIES}
		${QT_QTTEST_LIBRARY}
	)

	target_compile_features(${LMMS_TEST_NAME} PRIVATE cxx_std_20)
	target_compile_definitions(${LMMS_TEST_NAME} PRIVATE LMMS_TESTING)
	if(NOT WIN32)
		# These executables can also host native modules. Keep their core
		# class visibility consistent with lmmsobjs across that boundary.
		target_compile_options(${LMMS_TEST_NAME} PRIVATE -ULMMS_STATIC_DEFINE)
	endif()
endforeach()

set_tests_properties(StudioRuntimeTest PROPERTIES TIMEOUT 300)
set_tests_properties(SampleClipResourceTest PROPERTIES TIMEOUT 300 LABELS "headless;samples;resources;serialization;recovery")
set_tests_properties(StudioProjectResourceTest PROPERTIES TIMEOUT 300 LABELS "headless;resources;inspection;agent;recovery")
set_tests_properties(StudioResourceNameTest PROPERTIES TIMEOUT 300 LABELS "headless;resources;inspection;lifetimes;recovery")
set_tests_properties(FileSearchTest PROPERTIES TIMEOUT 180 LABELS "headless;filesystem;native-browser;resources")
set_tests_properties(StudioAssetsTest PROPERTIES TIMEOUT 300 LABELS "headless;filesystem;resources;agent;recovery")
set_tests_properties(LfoWaveResourceTest PROPERTIES TIMEOUT 300 LABELS "headless;controllers;resources;serialization;agent;recovery")
set_tests_properties(EnvelopeWaveResourceTest PROPERTIES TIMEOUT 300 LABELS "headless;instruments;modulation;resources;serialization;agent;recovery")
set_tests_properties(StudioAudioIdentityTest PROPERTIES TIMEOUT 600)
set_tests_properties(StudioExportSettingsTest PROPERTIES TIMEOUT 600)
set_tests_properties(StudioStemTest PROPERTIES TIMEOUT 600)
set_tests_properties(TrackRenderScopeLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;render;stems;lifetimes;recovery")
set_tests_properties(RenderManagerPreparationTest PROPERTIES TIMEOUT 300 LABELS "headless;render;stems;lifetimes;recovery")
set_tests_properties(RenderManagerLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;render;stems;lifetimes;recovery")
set_tests_properties(RenderManagerConstructionTest PROPERTIES TIMEOUT 300 LABELS "headless;render;stems;lifetimes;recovery")
set_tests_properties(RenderManagerPublicationTest PROPERTIES TIMEOUT 300 LABELS "headless;render;stems;lifetimes;recovery;publication;codecs")
set_tests_properties(NativeAudioEncoderTest PROPERTIES TIMEOUT 300 LABELS "headless;render;codecs;resources;recovery")
set_tests_properties(ProjectRendererFailureTest PROPERTIES TIMEOUT 600 LABELS "headless;render;codecs;agent;recovery")
set_tests_properties(StudioRenderOwnershipTest PROPERTIES TIMEOUT 600 LABELS "headless;render;agent;lifetimes;recovery;publication")
set_tests_properties(StudioRenderCancellationTest PROPERTIES TIMEOUT 300 LABELS "headless;render;agent;callbacks;recovery")
set_tests_properties(SongExportTransitionTest PROPERTIES TIMEOUT 600 LABELS "headless;render;agent;transport;recovery")
set_tests_properties(StudioRecoveryTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioSessionReentryTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioContextTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioToolResultTest PROPERTIES TIMEOUT 240)
set_tests_properties(StudioReceiptTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioTranscriptTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioShutdownTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioStorageTest PROPERTIES TIMEOUT 180)
set_tests_properties(StudioArchivePathTest PROPERTIES TIMEOUT 240)
set_tests_properties(StudioResourcesTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioResourceBudgetTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioBackendTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioBackendSelectionTest PROPERTIES TIMEOUT 600)
if(LMMS_HAVE_SDL)
	target_link_libraries(StudioBackendSelectionTest PRIVATE SDL2::SDL2)
endif()
set_tests_properties(AudioEngineFaultTest PROPERTIES TIMEOUT 120)
set_tests_properties(DspCacheRecoveryTest PROPERTIES TIMEOUT 240 LABELS "headless;dsp;controllers;recovery")
set_tests_properties(LinkedParameterTest PROPERTIES TIMEOUT 300 LABELS "headless;parameters;controllers;inspection")
set_tests_properties(LinkedProjectPersistenceTest PROPERTIES TIMEOUT 300 LABELS "headless;parameters;persistence;recovery")
set_tests_properties(LinkedTrackPersistenceTest PROPERTIES TIMEOUT 300 LABELS "headless;parameters;presets;clone;recovery")
set_tests_properties(LinkedClipPersistenceTest PROPERTIES TIMEOUT 300 LABELS "headless;parameters;clips;clone;recovery")
set_tests_properties(LinkedInlinePersistenceTest PROPERTIES TIMEOUT 300 LABELS "headless;parameters;inline;persistence;copy;recovery")
set_tests_properties(NativeClipTransferTest PROPERTIES TIMEOUT 300 LABELS "headless;clipboard;clips;recovery")
set_tests_properties(NativeNoteTransferTest PROPERTIES TIMEOUT 300 LABELS "headless;notes;clipboard;copy;journal;recovery")
set_tests_properties(StudioClipSelectionCopyTest PROPERTIES TIMEOUT 300 LABELS "headless;clips;agent;history;recovery")
set_tests_properties(StudioNoteSelectionCopyTest PROPERTIES TIMEOUT 300 LABELS "headless;notes;agent;history;recovery")
set_tests_properties(StudioNoteSelectionShiftTest PROPERTIES TIMEOUT 300 LABELS "headless;notes;agent;history;recovery")
set_tests_properties(StudioNoteSelectionQuantizeTest PROPERTIES TIMEOUT 300 LABELS "headless;notes;agent;history;recovery")
set_tests_properties(StudioNoteSelectionReverseTest PROPERTIES TIMEOUT 300 LABELS "headless;notes;agent;history;recovery")
set_tests_properties(StudioNoteSelectionLengthTest PROPERTIES TIMEOUT 300 LABELS "headless;notes;agent;history;recovery")
set_tests_properties(StudioNoteSelectionGlueTest PROPERTIES TIMEOUT 300 LABELS "headless;notes;agent;history;recovery")
set_tests_properties(StudioNoteSelectionRemoveTest PROPERTIES TIMEOUT 300 LABELS "headless;notes;agent;history;recovery")
set_tests_properties(StudioNoteSelectionEditTest PROPERTIES TIMEOUT 300 LABELS "headless;notes;agent;history;recovery")
set_tests_properties(StudioNoteBatchAddTest PROPERTIES TIMEOUT 300 LABELS "headless;notes;agent;history;recovery")
set_tests_properties(StudioCapabilitySchemaTest PROPERTIES TIMEOUT 120 LABELS "headless;schema;preflight")
set_tests_properties(StudioCapabilityPreflightTest PROPERTIES TIMEOUT 300 LABELS "headless;schema;agent;history;recovery")
set_tests_properties(StudioClipEditTest PROPERTIES TIMEOUT 300 LABELS "headless;clips;agent;history;recovery")
set_tests_properties(StudioTrackSoloTest PROPERTIES TIMEOUT 300 LABELS "headless;solo;agent;history;recovery")
set_tests_properties(StudioTrackLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;tracks;agent;history;recovery")
set_tests_properties(PatternQueryTest PROPERTIES TIMEOUT 300 LABELS "headless;patterns;automation;playback;queries")
set_tests_properties(StudioNoteSelectionSplitTest PROPERTIES TIMEOUT 300 LABELS "headless;notes;agent;history;recovery")
set_tests_properties(StudioNoteSelectionKnifeTest PROPERTIES TIMEOUT 300 LABELS "headless;notes;agent;history;recovery")
set_tests_properties(StudioStepCloneTest PROPERTIES TIMEOUT 300 LABELS "headless;steps;agent;history;recovery")
set_tests_properties(StudioStepResizeTest PROPERTIES TIMEOUT 300 LABELS "headless;steps;agent;history;recovery")
set_tests_properties(StudioStepSetTest PROPERTIES TIMEOUT 300 LABELS "headless;steps;agent;history;recovery")
set_tests_properties(StudioParameterLinkTest PROPERTIES TIMEOUT 300 LABELS "headless;parameters;agent;history;recovery")
set_tests_properties(LinkedValueOwnershipTest PROPERTIES TIMEOUT 300 LABELS "headless;parameters;lifetime;recovery")
set_tests_properties(ModelSetterOwnershipTest PROPERTIES TIMEOUT 300 LABELS "headless;parameters;tempo;lifetime;recovery")
set_tests_properties(ModelValueNotificationTest PROPERTIES TIMEOUT 300 LABELS "headless;parameters;notifications;lifetimes;recovery")
set_tests_properties(NativeValueNotificationTest PROPERTIES TIMEOUT 300 LABELS "headless;parameters;controllers;midi;notifications;recovery")
set_tests_properties(NativeParameterConversionTest PROPERTIES TIMEOUT 600 LABELS "headless;parameters;conversion;controllers;render;recovery")
set_tests_properties(StudioParameterReadbackTest PROPERTIES TIMEOUT 600 LABELS "headless;parameters;inspection;controllers;recovery;persistence;render")
set_tests_properties(StudioCapabilityCancellationTest PROPERTIES TIMEOUT 300 LABELS "headless;capabilities;cancellation;parameters;recovery;persistence")
set_tests_properties(TimeSignatureSafetyTest PROPERTIES TIMEOUT 600 LABELS "headless;meter;arithmetic;notifications;recovery;persistence;render")
set_tests_properties(LinkedValueJournalTest PROPERTIES TIMEOUT 300 LABELS "headless;parameters;journal;recovery")
set_tests_properties(ControlLinkJournalTest PROPERTIES TIMEOUT 300 LABELS "headless;parameters;topology;journal;recovery")
set_tests_properties(NativeGraphTransitionTest PROPERTIES TIMEOUT 300 LABELS "headless;parameters;topology;persistence;recovery")
set_tests_properties(AudioBusOutputTest PROPERTIES TIMEOUT 180)
set_tests_properties(AudioEngineWorkerTest PROPERTIES TIMEOUT 120)
set_tests_properties(MixerProcessingTest PROPERTIES TIMEOUT 120)
set_tests_properties(MixerSoloTest PROPERTIES TIMEOUT 120)
set_tests_properties(MixerStructureTest PROPERTIES TIMEOUT 120)
set_tests_properties(MixerLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;mixer;lifetime;restoration;recovery")
set_tests_properties(EffectChainTest PROPERTIES TIMEOUT 120)
if(TARGET amplifier)
	add_dependencies(EffectChainTest amplifier)
	target_compile_definitions(EffectChainTest PRIVATE LMMS_TEST_HAVE_AMPLIFIER)
endif()
set_tests_properties(EffectChainLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;effects;editing;history;recovery")
set_tests_properties(StudioEffectControlsTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioParameterPagingTest PROPERTIES TIMEOUT 180)
set_tests_properties(StudioProjectInspectionTest PROPERTIES TIMEOUT 240)
set_tests_properties(StudioWarningInspectionTest PROPERTIES TIMEOUT 240 LABELS "headless;diagnostics;recovery;agent")
set_tests_properties(StudioMixInspectionTest PROPERTIES TIMEOUT 600)
set_tests_properties(StudioAgentRestoreTest PROPERTIES TIMEOUT 300)
set_tests_properties(StudioSelectionTest PROPERTIES TIMEOUT 600)
set_tests_properties(StudioReplanningTest PROPERTIES TIMEOUT 600)
set_tests_properties(StudioHistoryIndexTest PROPERTIES TIMEOUT 600)
set_tests_properties(JournalIdAllocatorTest PROPERTIES TIMEOUT 120)
set_tests_properties(GraphModelTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioGraphTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioExpressionTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioPluginProgramTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioPluginPresetTest PROPERTIES TIMEOUT 120)
target_include_directories(StudioPluginPresetTest PRIVATE ../plugins/VstBase)
set_tests_properties(VstProgramsTest PROPERTIES TIMEOUT 120)
target_include_directories(VstProgramsTest PRIVATE ../plugins/VstBase)
set_tests_properties(VstParameterWireTest PROPERTIES TIMEOUT 120)
target_include_directories(VstParameterWireTest PRIVATE ../plugins/VstBase)
set_tests_properties(VstParameterTransferTest PROPERTIES TIMEOUT 120)
target_include_directories(VstParameterTransferTest PRIVATE ../plugins/VstBase)
set_tests_properties(VstPeFileTest PROPERTIES TIMEOUT 120)
target_include_directories(VstPeFileTest PRIVATE ../plugins/VstBase)
set_tests_properties(VstMidiBufferTest PROPERTIES TIMEOUT 120)
target_include_directories(VstMidiBufferTest PRIVATE ../plugins/VstBase)
set_tests_properties(VstPresetTest PROPERTIES TIMEOUT 120)
target_include_directories(VstPresetTest PRIVATE ../plugins/VstBase)
set_tests_properties(VstPresetStorageTest PROPERTIES TIMEOUT 120)
target_include_directories(VstPresetStorageTest PRIVATE ../plugins/VstBase)
set_tests_properties(MixerViewLifecycleTest PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(AudioDeviceTransitionTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioRecordingTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioMidiRecordingTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioAutomationRecordingTest PROPERTIES TIMEOUT 120)
set_tests_properties(AutomationTakeCommitTest PROPERTIES TIMEOUT 300 LABELS "headless;automation;recording;recovery")
set_tests_properties(AutomationTakeReservationTest PROPERTIES TIMEOUT 300 LABELS "headless;automation;recording;recovery")
set_tests_properties(StudioCurveEditTest PROPERTIES TIMEOUT 300 LABELS "headless;automation;editing;history;recovery")
set_tests_properties(StudioCurveEraseTest PROPERTIES TIMEOUT 300 LABELS "headless;automation;editing;history;recovery")
set_tests_properties(StudioAutomationBindingTest PROPERTIES TIMEOUT 300 LABELS "headless;automation;editing;history;recovery")
set_tests_properties(StudioControllerDisconnectTest PROPERTIES TIMEOUT 300 LABELS "headless;controllers;editing;history;recovery")
set_tests_properties(StudioMidiAssignmentTest PROPERTIES TIMEOUT 300 LABELS "headless;controllers;midi;editing;history;recovery")
set_tests_properties(StudioControllerAssignmentTest PROPERTIES TIMEOUT 300 LABELS "headless;controllers;editing;history;recovery")
set_tests_properties(StudioControllerLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;controllers;editing;history;recovery")
set_tests_properties(SongControllerNotificationTest PROPERTIES TIMEOUT 300 LABELS "headless;controllers;notifications;recovery")
set_tests_properties(StudioMidiControllerTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioMidiLearnTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioTrackMoveTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioPatternTest PROPERTIES TIMEOUT 120)
set_tests_properties(InstrumentMidiCCTest PROPERTIES TIMEOUT 120 LABELS "native-midi;headless;controllers;recovery")
set_tests_properties(InstrumentMidiEventTest PROPERTIES TIMEOUT 180 LABELS "native-midi;headless;events")
set_tests_properties(InstrumentLoadLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;instruments;editing;history;recovery")
set_tests_properties(InstrumentPresetLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;presets;instruments;serialization;history;recovery")
set_tests_properties(InstrumentPresetSaveTest PROPERTIES TIMEOUT 300 LABELS "headless;presets;serialization;files;history;recovery")
set_tests_properties(ProjectSaveLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;projects;serialization;files;history;recovery")
set_tests_properties(ProjectLoadLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;projects;restoration;history;recovery")
set_tests_properties(ProjectCreationLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;projects;creation;templates;history;recovery")
set_tests_properties(ImportFilterLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;projects;import;lifetime;history;recovery")
if(TARGET midiimport)
	add_dependencies(ImportFilterLifecycleTest midiimport)
endif()
if(TARGET tripleoscillator AND TARGET kicker)
	add_dependencies(ProjectCreationLifecycleTest tripleoscillator kicker)
endif()
set_tests_properties(MidiInputRenderTest PROPERTIES TIMEOUT 240 LABELS "native-midi;headless;render;recovery")
set_tests_properties(SongExportRecoveryTest PROPERTIES TIMEOUT 240 LABELS "headless;render;transport;recovery")
set_tests_properties(SongAutomationOwnershipTest PROPERTIES TIMEOUT 180 LABELS "headless;automation;lifetime;recovery")
set_tests_properties(SongAutomationInterruptionTest PROPERTIES TIMEOUT 300 LABELS "headless;automation;transport;ownership")
set_tests_properties(ControllerConnectionRestoreTest PROPERTIES TIMEOUT 180 LABELS "headless;controllers;compatibility;recovery")
set_tests_properties(LfoCatchUpTest PROPERTIES TIMEOUT 300 LABELS "headless;controllers;render;timing")
set_tests_properties(LfoRateNotificationTest PROPERTIES TIMEOUT 600 LABELS "headless;controllers;render;timing;recovery")
set_tests_properties(TempoNotificationRecoveryTest PROPERTIES TIMEOUT 600 LABELS "headless;controllers;tempo;meter;recovery;persistence")
set_tests_properties(ControllerStateTest PROPERTIES TIMEOUT 240 LABELS "headless;controllers;lifetime;timing")
set_tests_properties(ControllerSettingsLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;controllers;lifetime;resources;recovery")
set_tests_properties(MidiPortLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;midi;lifetime;resources;recovery")
set_tests_properties(StudioClipCopyTest PROPERTIES TIMEOUT 120)
set_tests_properties(StudioTrackCloneTest PROPERTIES TIMEOUT 120)
set_tests_properties(MidiPortTest PROPERTIES TIMEOUT 120)
set_tests_properties(MidiClientTransferTest PROPERTIES TIMEOUT 180 LABELS "native-midi;headless;recovery")
set_tests_properties(MidiClientRetirementTest PROPERTIES TIMEOUT 180 LABELS "native-midi;headless;recovery")
set_tests_properties(RawMidiOutputTest PROPERTIES TIMEOUT 120 LABELS "native-midi;headless;recovery")
if(TARGET MidiExportTest)
	# Compile the production exporter directly, including its normal descriptor.
	target_sources(MidiExportTest PRIVATE ../plugins/MidiExport/MidiExport.cpp ../plugins/MidiExport/MidiFile.cpp)
	target_include_directories(MidiExportTest PRIVATE ../plugins/MidiExport $<TARGET_PROPERTY:midiexport,INCLUDE_DIRECTORIES>)
	target_compile_definitions(MidiExportTest PRIVATE PLUGIN_NAME=midiexport PLUGIN_STATIC_DEFINE)
	set_tests_properties(MidiExportTest PROPERTIES TIMEOUT 180 LABELS "native-midi;headless;export")
endif()
if(TARGET AudioFileProcessorLifecycleTest)
	target_sources(AudioFileProcessorLifecycleTest PRIVATE
		../plugins/AudioFileProcessor/AudioFileProcessor.cpp ../plugins/AudioFileProcessor/AudioFileProcessor.h
		../plugins/AudioFileProcessor/AudioFileProcessorView.cpp ../plugins/AudioFileProcessor/AudioFileProcessorView.h
		../plugins/AudioFileProcessor/AudioFileProcessorWaveView.cpp ../plugins/AudioFileProcessor/AudioFileProcessorWaveView.h)
	target_include_directories(AudioFileProcessorLifecycleTest PRIVATE ../plugins/AudioFileProcessor $<TARGET_PROPERTY:audiofileprocessor,INCLUDE_DIRECTORIES>)
	target_compile_definitions(AudioFileProcessorLifecycleTest PRIVATE PLUGIN_NAME=audiofileprocessor PLUGIN_STATIC_DEFINE)
	add_dependencies(AudioFileProcessorLifecycleTest audiofileprocessor)
	set_tests_properties(AudioFileProcessorLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;instruments;sampler;resources;serialization;recovery")
endif()
if(TARGET TripleOscillatorResourceTest)
	target_sources(TripleOscillatorResourceTest PRIVATE ../plugins/TripleOscillator/TripleOscillator.cpp ../plugins/TripleOscillator/TripleOscillator.h)
	target_include_directories(TripleOscillatorResourceTest PRIVATE ../plugins/TripleOscillator $<TARGET_PROPERTY:tripleoscillator,INCLUDE_DIRECTORIES>)
	target_compile_definitions(TripleOscillatorResourceTest PRIVATE PLUGIN_NAME=tripleoscillator PLUGIN_STATIC_DEFINE)
	add_dependencies(TripleOscillatorResourceTest tripleoscillator)
	set_target_properties(TripleOscillatorResourceTest PROPERTIES ENABLE_EXPORTS ON)
	set_tests_properties(TripleOscillatorResourceTest PROPERTIES TIMEOUT 300 LABELS "headless;instruments;resources;serialization;recovery;agent" ENVIRONMENT "LMMS_PLUGIN_DIR=$<TARGET_FILE_DIR:tripleoscillator>")
endif()
if(TARGET HydrogenImportTest)
	target_sources(HydrogenImportTest PRIVATE ../plugins/HydrogenImport/HydrogenImport.cpp ../plugins/HydrogenImport/HydrogenImport.h ../plugins/HydrogenImport/LocalFileMng.cpp)
	target_include_directories(HydrogenImportTest PRIVATE ../plugins/HydrogenImport $<TARGET_PROPERTY:hydrogenimport,INCLUDE_DIRECTORIES>)
	target_compile_definitions(HydrogenImportTest PRIVATE PLUGIN_NAME=hydrogenimport PLUGIN_STATIC_DEFINE)
	add_dependencies(HydrogenImportTest hydrogenimport audiofileprocessor)
	set_tests_properties(HydrogenImportTest PROPERTIES TIMEOUT 300 LABELS "headless;hydrogen;import;projects;assets;recovery")
endif()
if(TARGET MidiImportTest)
	target_sources(MidiImportTest PRIVATE ../plugins/MidiImport/MidiImport.cpp ../plugins/MidiImport/MidiImport.h
		../plugins/MidiImport/MidiImportData.cpp
		../plugins/MidiImport/portsmf/src/allegro.cpp ../plugins/MidiImport/portsmf/src/mfmidi.cpp
		../plugins/MidiImport/portsmf/src/strparse.cpp ../plugins/MidiImport/portsmf/src/allegrosmfrd.cpp
		../plugins/MidiImport/portsmf/src/allegrord.cpp)
	target_include_directories(MidiImportTest PRIVATE ../plugins/MidiImport $<TARGET_PROPERTY:midiimport,INCLUDE_DIRECTORIES>)
	target_compile_definitions(MidiImportTest PRIVATE PLUGIN_NAME=midiimport PLUGIN_STATIC_DEFINE)
	if(NOT MSVC)
		target_compile_options(MidiImportTest PRIVATE -Wno-error=unknown-pragmas -Wno-error=maybe-uninitialized -Wno-error=restrict)
	endif()
	set_tests_properties(MidiImportTest PROPERTIES TIMEOUT 180 LABELS "native-midi;headless;import;recovery")
endif()
if(LMMS_HAVE_WINMM)
	target_link_libraries(WinMmMidiIoTest PRIVATE winmm)
	set_tests_properties(WinMmMidiIoTest PROPERTIES TIMEOUT 120 LABELS "native-midi;winmm;headless;recovery")
endif()
if(LMMS_BUILD_APPLE)
	find_library(LMMS_TEST_COREMIDI_FRAMEWORK CoreMIDI)
	find_library(LMMS_TEST_COREFOUNDATION_FRAMEWORK CoreFoundation)
	target_link_libraries(MidiAppleTest PRIVATE ${LMMS_TEST_COREMIDI_FRAMEWORK} ${LMMS_TEST_COREFOUNDATION_FRAMEWORK})
	set_tests_properties(MidiAppleTest PROPERTIES TIMEOUT 120)
endif()
if(LMMS_HAVE_ALSA)
	target_link_libraries(MidiAlsaSeqTest PRIVATE ${ASOUND_LIBRARY})
	target_link_libraries(RawMidiIoTest PRIVATE ${ASOUND_LIBRARY})
	set_tests_properties(MidiAlsaSeqTest PROPERTIES TIMEOUT 120)
	target_link_libraries(AudioAlsaTest PRIVATE ${ASOUND_LIBRARY})
	set_tests_properties(AudioAlsaTest PROPERTIES TIMEOUT 180 LABELS "native-audio;alsa;headless;recovery")
endif()
set_tests_properties(StudioProtocolOutputTest PROPERTIES TIMEOUT 30)
if(LMMS_HAVE_PORTAUDIO)
	target_link_libraries(PortAudioCallbackTest PRIVATE portaudio::portaudio)
	set_tests_properties(PortAudioCallbackTest PROPERTIES TIMEOUT 120)
endif()
if(LMMS_HAVE_PULSEAUDIO)
	target_link_libraries(PulseAudioTest PRIVATE ${PULSEAUDIO_LIBRARIES})
	set_tests_properties(PulseAudioTest PROPERTIES TIMEOUT 180 LABELS "native-audio;pulseaudio;headless;recovery")
endif()
if(LMMS_HAVE_SNDIO)
	target_link_libraries(AudioSndioTest PRIVATE ${SNDIO_LIBRARIES})
	target_link_libraries(RawMidiIoTest PRIVATE ${SNDIO_LIBRARIES})
	set_tests_properties(AudioSndioTest PROPERTIES TIMEOUT 300 LABELS "native-audio;sndio;headless;recovery")
endif()
if(LMMS_HAVE_JACK)
	target_link_libraries(AudioJackTest PRIVATE ${JACK_LIBRARIES})
	set_tests_properties(AudioJackTest PROPERTIES TIMEOUT 180 LABELS "native-audio;jack;headless;recovery")
	target_link_libraries(JackLifetimeTest PRIVATE ${JACK_LIBRARIES})
	set_tests_properties(JackLifetimeTest PROPERTIES TIMEOUT 240 LABELS "native-audio;native-midi;jack;headless;recovery")
endif()
set_tests_properties(StudioAssetStoreTest PROPERTIES TIMEOUT 60)
if(LMMS_HAVE_ALSA OR LMMS_HAVE_SNDIO)
	set_tests_properties(RawMidiIoTest PROPERTIES TIMEOUT 180 LABELS "native-midi;headless;recovery")
endif()
set_tests_properties(FileResourceSnapshotTest PROPERTIES TIMEOUT 60)
set_tests_properties(GigPcmTest PROPERTIES TIMEOUT 60)
set_tests_properties(FileResourceIOTest PROPERTIES TIMEOUT 60)
set_tests_properties(DrumSynthTest PROPERTIES TIMEOUT 120)
set_tests_properties(SampleBufferTest PROPERTIES TIMEOUT 60)
set_tests_properties(SampleRecordingBufferTest PROPERTIES TIMEOUT 60)
target_compile_definitions(DrumSynthTest PRIVATE LMMS_TEST_DRUMSYNTH_DIR="${PROJECT_SOURCE_DIR}/data/samples/drumsynth")
target_compile_definitions(StudioRuntimeTest PRIVATE LMMS_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_compile_definitions(StudioRuntimeTest PRIVATE LMMS_TEST_DRUMSYNTH_DIR="${PROJECT_SOURCE_DIR}/data/samples/drumsynth")
set_tests_properties(StudioInferenceTest PROPERTIES TIMEOUT 60)
set_tests_properties(RemotePluginTest PROPERTIES TIMEOUT 90)
if(TARGET XpressiveExpressionTest)
	target_sources(XpressiveExpressionTest PRIVATE ../plugins/Xpressive/ExprSynth.cpp)
	target_include_directories(XpressiveExpressionTest PRIVATE ../plugins/Xpressive)
	target_link_libraries(XpressiveExpressionTest PRIVATE exprtk)
	set_tests_properties(XpressiveExpressionTest PROPERTIES TIMEOUT 120)
endif()
if(TARGET VstParameterModelsTest)
	target_sources(VstParameterModelsTest PRIVATE ../plugins/VstBase/VstParameterModels.cpp)
	target_include_directories(VstParameterModelsTest PRIVATE $<TARGET_PROPERTY:vstbase,INCLUDE_DIRECTORIES>)
	target_compile_definitions(VstParameterModelsTest PRIVATE VSTBASE_STATIC_DEFINE)
	set_tests_properties(VstParameterModelsTest PROPERTIES TIMEOUT 120)
endif()
if(TARGET VstNativeWorkerTest)
	find_package(Threads REQUIRED)
	foreach(kind IN ITEMS 0 1 2 3)
		add_library(VstNativeFixture${kind} MODULE fixtures/native-vst/NativeVstFixture.cpp)
		target_include_directories(VstNativeFixture${kind} PRIVATE ../include)
		target_compile_features(VstNativeFixture${kind} PRIVATE cxx_std_20)
		target_compile_definitions(VstNativeFixture${kind} PRIVATE LMMS_VST_FIXTURE_KIND=${kind})
		target_link_libraries(VstNativeFixture${kind} PRIVATE Threads::Threads)
		set_target_properties(VstNativeFixture${kind} PROPERTIES AUTOMOC OFF PREFIX ""
			LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/native-vst-fixtures")
		target_compile_definitions(VstNativeWorkerTest PRIVATE LMMS_TEST_VST_FIXTURE_${kind}="$<TARGET_FILE:VstNativeFixture${kind}>")
		add_dependencies(VstNativeWorkerTest VstNativeFixture${kind})
	endforeach()
	# Compile production host sources into the test's engine instance, as for
	# VstParameterModelsTest, rather than linking a second copy of the DAW core.
	target_sources(VstNativeWorkerTest PRIVATE ../plugins/VstBase/VstPlugin.cpp ../plugins/VstBase/VstPlugin.h
		../plugins/VstBase/VstParameterModels.cpp ../plugins/VstBase/VstParameterModels.h)
	target_include_directories(VstNativeWorkerTest PRIVATE $<TARGET_PROPERTY:vstbase,INCLUDE_DIRECTORIES>)
	target_compile_definitions(VstNativeWorkerTest PRIVATE VSTBASE_STATIC_DEFINE
		REMOTE_VST_PLUGIN_FILEPATH_32="${REMOTE_VST_PLUGIN_FILEPATH_32}"
		REMOTE_VST_PLUGIN_FILEPATH_64="${REMOTE_VST_PLUGIN_FILEPATH_64}"
		LMMS_TEST_VST_WORKER_DIR="${PROJECT_BINARY_DIR}/plugins")
	if(LMMS_BUILD_LINUX)
		target_compile_definitions(VstNativeWorkerTest PRIVATE NATIVE_LINUX_REMOTE_VST_PLUGIN_FILEPATH_64="${NATIVE_LINUX_REMOTE_VST_PLUGIN_FILEPATH_64}")
		add_dependencies(VstNativeWorkerTest NativeLinuxRemoteVstPlugin64)
		if(NOT WANT_QT6)
			target_link_libraries(VstNativeWorkerTest PRIVATE qx11embedcontainer)
		endif()
	elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
		add_dependencies(VstNativeWorkerTest RemoteVstPlugin64)
	else()
		add_dependencies(VstNativeWorkerTest RemoteVstPlugin32)
	endif()
	set_tests_properties(VstNativeWorkerTest PROPERTIES TIMEOUT 360 LABELS "native-vst;worker;headless")
endif()
if(TARGET CarlaHostTest)
	# Exercise the actual LMMS host with deterministic native Carla callbacks.
	# No Carla library is linked: this fixture supplies native entry points.
	target_sources(CarlaHostTest PRIVATE
		../plugins/CarlaBase/Carla.cpp ../plugins/CarlaBase/Carla.h
		../plugins/CarlaBase/CarlaPluginHost.cpp ../plugins/CarlaBase/CarlaPluginHost.h
		../plugins/CarlaBase/CarlaPluginDiscovery.cpp ../plugins/CarlaBase/CarlaPluginDiscovery.h
		../plugins/CarlaBase/CarlaPluginPatchbay.cpp ../plugins/CarlaBase/CarlaPluginPatchbay.h)
	target_include_directories(CarlaHostTest PRIVATE
		$<TARGET_PROPERTY:carlabase,INCLUDE_DIRECTORIES>)
	target_compile_definitions(CarlaHostTest PRIVATE
		CARLABASE_STATIC_DEFINE BUILDING_CARLA PLUGIN_NAME=carlahosttest
		$<TARGET_PROPERTY:carlabase,INTERFACE_COMPILE_DEFINITIONS>)
	set_tests_properties(CarlaHostTest PROPERTIES TIMEOUT 120)
endif()
if(TARGET CarlaPluginDiscoveryTest)
	target_sources(CarlaPluginDiscoveryTest PRIVATE
		../plugins/CarlaBase/CarlaPluginDiscovery.cpp ../plugins/CarlaBase/CarlaPluginDiscovery.h)
	target_include_directories(CarlaPluginDiscoveryTest PRIVATE $<TARGET_PROPERTY:carlabase,INCLUDE_DIRECTORIES>)
	target_compile_definitions(CarlaPluginDiscoveryTest PRIVATE BUILDING_CARLA
		$<TARGET_PROPERTY:carlabase,INTERFACE_COMPILE_DEFINITIONS>)
	set_tests_properties(CarlaPluginDiscoveryTest PROPERTIES TIMEOUT 60)
endif()
if(TARGET CarlaPluginPatchbayTest)
	target_sources(CarlaPluginPatchbayTest PRIVATE
		../plugins/CarlaBase/CarlaPluginPatchbay.cpp ../plugins/CarlaBase/CarlaPluginPatchbay.h)
	target_include_directories(CarlaPluginPatchbayTest PRIVATE $<TARGET_PROPERTY:carlabase,INCLUDE_DIRECTORIES>)
	target_compile_definitions(CarlaPluginPatchbayTest PRIVATE BUILDING_CARLA
		$<TARGET_PROPERTY:carlabase,INTERFACE_COMPILE_DEFINITIONS>)
	set_tests_properties(CarlaPluginPatchbayTest PROPERTIES TIMEOUT 60)
endif()
if(LMMS_HAVE_GIG)
	target_include_directories(StudioRuntimeTest SYSTEM PRIVATE ${GIG_INCLUDE_DIRS})
	target_link_directories(StudioRuntimeTest PRIVATE ${GIG_LIBRARY_DIRS})
	target_link_libraries(StudioRuntimeTest PRIVATE ${GIG_LIBRARIES})
	target_compile_definitions(StudioRuntimeTest PRIVATE LMMS_TEST_LIBGIG)
endif()
if(LMMS_HAVE_FLUIDSYNTH)
	target_link_libraries(Sf2SynthStateTest PRIVATE fluidsynth)
	target_compile_definitions(Sf2SynthStateTest PRIVATE LMMS_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
	set_tests_properties(Sf2SynthStateTest PROPERTIES TIMEOUT 60)
endif()
if(TARGET GigResourceLifecycleTest)
	target_sources(GigResourceLifecycleTest PRIVATE
		../plugins/GigPlayer/GigPlayer.cpp ../plugins/GigPlayer/GigPlayer.h
		../plugins/GigPlayer/PatchesDialog.cpp ../plugins/GigPlayer/PatchesDialog.h ../plugins/GigPlayer/PatchesDialog.ui)
	target_include_directories(GigResourceLifecycleTest PRIVATE ../plugins/GigPlayer $<TARGET_PROPERTY:gigplayer,INCLUDE_DIRECTORIES>)
	target_include_directories(GigResourceLifecycleTest SYSTEM PRIVATE ${GIG_INCLUDE_DIRS})
	target_link_directories(GigResourceLifecycleTest PRIVATE ${GIG_LIBRARY_DIRS})
	target_compile_definitions(GigResourceLifecycleTest PRIVATE PLUGIN_NAME=gigplayer PLUGIN_STATIC_DEFINE)
	target_link_libraries(GigResourceLifecycleTest PRIVATE ${GIG_LIBRARIES} SampleRate::samplerate)
	set_target_properties(GigResourceLifecycleTest PROPERTIES AUTOMOC ON AUTOUIC ON AUTOUIC_SEARCH_PATHS "${PROJECT_SOURCE_DIR}/plugins/GigPlayer" ENABLE_EXPORTS ON)
	add_dependencies(GigResourceLifecycleTest gigplayer)
	set_tests_properties(GigResourceLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;gig;resources;serialization;lifetimes;recovery" ENVIRONMENT "LMMS_PLUGIN_DIR=$<TARGET_FILE_DIR:gigplayer>")
endif()
if(TARGET GigPendingBankTest)
	target_sources(GigPendingBankTest PRIVATE
		../plugins/GigPlayer/GigPlayer.cpp ../plugins/GigPlayer/GigPlayer.h
		../plugins/GigPlayer/PatchesDialog.cpp ../plugins/GigPlayer/PatchesDialog.h ../plugins/GigPlayer/PatchesDialog.ui)
	target_include_directories(GigPendingBankTest PRIVATE ../plugins/GigPlayer $<TARGET_PROPERTY:gigplayer,INCLUDE_DIRECTORIES>)
	target_include_directories(GigPendingBankTest SYSTEM PRIVATE ${GIG_INCLUDE_DIRS})
	target_link_directories(GigPendingBankTest PRIVATE ${GIG_LIBRARY_DIRS})
	target_compile_definitions(GigPendingBankTest PRIVATE PLUGIN_NAME=gigplayer PLUGIN_STATIC_DEFINE)
	target_link_libraries(GigPendingBankTest PRIVATE ${GIG_LIBRARIES} SampleRate::samplerate)
	set_target_properties(GigPendingBankTest PROPERTIES AUTOMOC ON AUTOUIC ON AUTOUIC_SEARCH_PATHS "${PROJECT_SOURCE_DIR}/plugins/GigPlayer" ENABLE_EXPORTS ON)
	add_dependencies(GigPendingBankTest gigplayer)
	set_tests_properties(GigPendingBankTest PROPERTIES TIMEOUT 300 LABELS "headless;gig;resources;serialization;lifetimes;recovery;pending" ENVIRONMENT "LMMS_PLUGIN_DIR=$<TARGET_FILE_DIR:gigplayer>")
endif()
if(TARGET GigPatchDialogTest)
	target_sources(GigPatchDialogTest PRIVATE
		../plugins/GigPlayer/GigPlayer.cpp ../plugins/GigPlayer/GigPlayer.h
		../plugins/GigPlayer/PatchesDialog.cpp ../plugins/GigPlayer/PatchesDialog.h ../plugins/GigPlayer/PatchesDialog.ui)
	target_include_directories(GigPatchDialogTest PRIVATE ../plugins/GigPlayer $<TARGET_PROPERTY:gigplayer,INCLUDE_DIRECTORIES>)
	target_include_directories(GigPatchDialogTest SYSTEM PRIVATE ${GIG_INCLUDE_DIRS})
	target_link_directories(GigPatchDialogTest PRIVATE ${GIG_LIBRARY_DIRS})
	target_compile_definitions(GigPatchDialogTest PRIVATE PLUGIN_NAME=gigplayer PLUGIN_STATIC_DEFINE)
	target_link_libraries(GigPatchDialogTest PRIVATE ${GIG_LIBRARIES} SampleRate::samplerate)
	set_target_properties(GigPatchDialogTest PROPERTIES AUTOMOC ON AUTOUIC ON AUTOUIC_SEARCH_PATHS "${PROJECT_SOURCE_DIR}/plugins/GigPlayer" ENABLE_EXPORTS ON)
	add_dependencies(GigPatchDialogTest gigplayer)
	set_tests_properties(GigPatchDialogTest PROPERTIES TIMEOUT 300 LABELS "gui;gig;programs;resources;lifetimes;recovery" ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LMMS_PLUGIN_DIR=$<TARGET_FILE_DIR:gigplayer>")
endif()
if(TARGET Sf2ResourceLifecycleTest)
	target_sources(Sf2ResourceLifecycleTest PRIVATE
		../plugins/Sf2Player/Sf2Player.cpp ../plugins/Sf2Player/Sf2Player.h
		../plugins/Sf2Player/PatchesDialog.cpp ../plugins/Sf2Player/PatchesDialog.h ../plugins/Sf2Player/PatchesDialog.ui)
	target_include_directories(Sf2ResourceLifecycleTest PRIVATE ../plugins/Sf2Player $<TARGET_PROPERTY:sf2player,INCLUDE_DIRECTORIES>)
	target_compile_definitions(Sf2ResourceLifecycleTest PRIVATE PLUGIN_NAME=sf2player PLUGIN_STATIC_DEFINE LMMS_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
	target_link_libraries(Sf2ResourceLifecycleTest PRIVATE fluidsynth SampleRate::samplerate)
	set_target_properties(Sf2ResourceLifecycleTest PROPERTIES AUTOMOC ON AUTOUIC ON AUTOUIC_SEARCH_PATHS "${PROJECT_SOURCE_DIR}/plugins/Sf2Player" ENABLE_EXPORTS ON)
	add_dependencies(Sf2ResourceLifecycleTest sf2player)
	set_tests_properties(Sf2ResourceLifecycleTest PROPERTIES TIMEOUT 300 LABELS "headless;soundfont;resources;serialization;lifetimes;recovery" ENVIRONMENT "LMMS_PLUGIN_DIR=$<TARGET_FILE_DIR:sf2player>")
endif()
if(TARGET Sf2RateFallbackTest)
	target_sources(Sf2RateFallbackTest PRIVATE
		../plugins/Sf2Player/Sf2Player.cpp ../plugins/Sf2Player/Sf2Player.h
		../plugins/Sf2Player/PatchesDialog.cpp ../plugins/Sf2Player/PatchesDialog.h ../plugins/Sf2Player/PatchesDialog.ui)
	target_include_directories(Sf2RateFallbackTest PRIVATE ../plugins/Sf2Player $<TARGET_PROPERTY:sf2player,INCLUDE_DIRECTORIES>)
	target_compile_definitions(Sf2RateFallbackTest PRIVATE PLUGIN_NAME=sf2player PLUGIN_STATIC_DEFINE LMMS_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
	target_link_libraries(Sf2RateFallbackTest PRIVATE fluidsynth SampleRate::samplerate)
	set_target_properties(Sf2RateFallbackTest PROPERTIES AUTOMOC ON AUTOUIC ON AUTOUIC_SEARCH_PATHS "${PROJECT_SOURCE_DIR}/plugins/Sf2Player" ENABLE_EXPORTS ON)
	add_dependencies(Sf2RateFallbackTest sf2player)
	set_tests_properties(Sf2RateFallbackTest PROPERTIES TIMEOUT 300 LABELS "headless;soundfont;resources;lifetimes;recovery;resampling" ENVIRONMENT "LMMS_PLUGIN_DIR=$<TARGET_FILE_DIR:sf2player>")
endif()
if(TARGET Sf2RenderAuditionTest)
	target_sources(Sf2RenderAuditionTest PRIVATE
		../plugins/Sf2Player/Sf2Player.cpp ../plugins/Sf2Player/Sf2Player.h
		../plugins/Sf2Player/PatchesDialog.cpp ../plugins/Sf2Player/PatchesDialog.h ../plugins/Sf2Player/PatchesDialog.ui)
	target_include_directories(Sf2RenderAuditionTest PRIVATE ../plugins/Sf2Player $<TARGET_PROPERTY:sf2player,INCLUDE_DIRECTORIES>)
	target_compile_definitions(Sf2RenderAuditionTest PRIVATE PLUGIN_NAME=sf2player PLUGIN_STATIC_DEFINE LMMS_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
	target_link_libraries(Sf2RenderAuditionTest PRIVATE fluidsynth SampleRate::samplerate)
	set_target_properties(Sf2RenderAuditionTest PROPERTIES AUTOMOC ON AUTOUIC ON AUTOUIC_SEARCH_PATHS "${PROJECT_SOURCE_DIR}/plugins/Sf2Player" ENABLE_EXPORTS ON)
	add_dependencies(Sf2RenderAuditionTest sf2player)
	set_tests_properties(Sf2RenderAuditionTest PROPERTIES TIMEOUT 300 LABELS "headless;soundfont;resources;serialization;lifetimes;recovery;render;preview" ENVIRONMENT "LMMS_PLUGIN_DIR=$<TARGET_FILE_DIR:sf2player>")
endif()
if(TARGET Sf2PendingBankTest)
	target_sources(Sf2PendingBankTest PRIVATE
		../plugins/Sf2Player/Sf2Player.cpp ../plugins/Sf2Player/Sf2Player.h
		../plugins/Sf2Player/PatchesDialog.cpp ../plugins/Sf2Player/PatchesDialog.h ../plugins/Sf2Player/PatchesDialog.ui)
	target_include_directories(Sf2PendingBankTest PRIVATE ../plugins/Sf2Player $<TARGET_PROPERTY:sf2player,INCLUDE_DIRECTORIES>)
	target_compile_definitions(Sf2PendingBankTest PRIVATE PLUGIN_NAME=sf2player PLUGIN_STATIC_DEFINE LMMS_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
	target_link_libraries(Sf2PendingBankTest PRIVATE fluidsynth SampleRate::samplerate)
	set_target_properties(Sf2PendingBankTest PROPERTIES AUTOMOC ON AUTOUIC ON AUTOUIC_SEARCH_PATHS "${PROJECT_SOURCE_DIR}/plugins/Sf2Player" ENABLE_EXPORTS ON)
	add_dependencies(Sf2PendingBankTest sf2player)
	set_tests_properties(Sf2PendingBankTest PROPERTIES TIMEOUT 300 LABELS "headless;soundfont;resources;serialization;lifetimes;recovery;pending" ENVIRONMENT "LMMS_PLUGIN_DIR=$<TARGET_FILE_DIR:sf2player>")
endif()
if(TARGET Sf2PatchDialogTest)
	target_sources(Sf2PatchDialogTest PRIVATE
		../plugins/Sf2Player/Sf2Player.cpp ../plugins/Sf2Player/Sf2Player.h
		../plugins/Sf2Player/PatchesDialog.cpp ../plugins/Sf2Player/PatchesDialog.h ../plugins/Sf2Player/PatchesDialog.ui)
	target_include_directories(Sf2PatchDialogTest PRIVATE ../plugins/Sf2Player $<TARGET_PROPERTY:sf2player,INCLUDE_DIRECTORIES>)
	target_compile_definitions(Sf2PatchDialogTest PRIVATE PLUGIN_NAME=sf2player PLUGIN_STATIC_DEFINE LMMS_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
	target_link_libraries(Sf2PatchDialogTest PRIVATE fluidsynth SampleRate::samplerate)
	set_target_properties(Sf2PatchDialogTest PROPERTIES AUTOMOC ON AUTOUIC ON AUTOUIC_SEARCH_PATHS "${PROJECT_SOURCE_DIR}/plugins/Sf2Player" ENABLE_EXPORTS ON)
	add_dependencies(Sf2PatchDialogTest sf2player)
	set_tests_properties(Sf2PatchDialogTest PROPERTIES TIMEOUT 300 LABELS "gui;soundfont;programs;preview;resources;lifetimes;recovery" ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LMMS_PLUGIN_DIR=$<TARGET_FILE_DIR:sf2player>")
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND NOT LMMS_MINIMAL)
	add_test(NAME StudioProcessTest
		COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/studio_process_test.py"
			--lmms $<TARGET_FILE:lmms> --output "${CMAKE_BINARY_DIR}/studio-evidence")
	set_tests_properties(StudioProcessTest PROPERTIES TIMEOUT 900)
	add_test(NAME StudioRenderProcessTest
		COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/studio_render_process_test.py"
			--lmms $<TARGET_FILE:lmms> --output "${CMAKE_BINARY_DIR}/studio-render-evidence")
	set_tests_properties(StudioRenderProcessTest PROPERTIES TIMEOUT 120 LABELS "headless;render;instruments;recovery")
	if(TARGET VstNativeWorkerTest AND TARGET vsteffect)
		add_custom_target(StudioVstProcessFixtures DEPENDS lmms vsteffect VstNativeFixture0 VstNativeFixture1 VstNativeFixture2 VstNativeFixture3)
		if(LMMS_BUILD_LINUX)
			add_dependencies(StudioVstProcessFixtures NativeLinuxRemoteVstPlugin64)
		elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
			add_dependencies(StudioVstProcessFixtures RemoteVstPlugin64)
		else()
			add_dependencies(StudioVstProcessFixtures RemoteVstPlugin32)
		endif()
		foreach(kind IN ITEMS 0 1 2)
			add_test(NAME StudioVstProcessTest${kind}
				COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/studio_vst_process_test.py"
					--lmms $<TARGET_FILE:lmms> --fixture $<TARGET_FILE:VstNativeFixture${kind}> --kind ${kind}
					--worker-root "${PROJECT_BINARY_DIR}/plugins" --native-effect $<TARGET_FILE:vsteffect> --vstbase $<TARGET_FILE:vstbase>
					--output "${CMAKE_BINARY_DIR}/studio-vst-evidence")
			set_tests_properties(StudioVstProcessTest${kind} PROPERTIES TIMEOUT 360 LABELS "native-vst;worker;headless;recovery")
		endforeach()
		add_test(NAME StudioVstDenseProcessTest
			COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/studio_vst_dense_process_test.py"
				--lmms $<TARGET_FILE:lmms> --fixture $<TARGET_FILE:VstNativeFixture3>
				--worker-root "${PROJECT_BINARY_DIR}/plugins" --native-effect $<TARGET_FILE:vsteffect> --vstbase $<TARGET_FILE:vstbase>
				--output "${CMAKE_BINARY_DIR}/studio-vst-evidence")
		set_tests_properties(StudioVstDenseProcessTest PROPERTIES TIMEOUT 900 LABELS "native-vst;worker;headless;recovery;large-layout")
	endif()
endif()

# Native tests live one directory below the executable and cannot discover its
# sibling plugin directory through the usual application-relative paths.
get_property(LMMS_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
set_property(TEST ${LMMS_REGISTERED_TESTS} APPEND PROPERTY ENVIRONMENT "LMMS_PLUGIN_DIR=${PROJECT_BINARY_DIR}/plugins")

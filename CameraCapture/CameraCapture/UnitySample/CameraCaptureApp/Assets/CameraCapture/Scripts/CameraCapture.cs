// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for license information.

using SpatialTranformHelper;
using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using UnityEngine;

namespace CameraCapture
{
    internal class CameraCapture : BasePlugin<CameraCapture>
    {
        public Int32 Width = 1280;
        public Int32 Height = 720;
        public Boolean EnableAudio = false;
        public Boolean EnableMrc = false;
        public Boolean EnabledPreview = false;
        public SpatialCameraTracker CameraTracker = null;

        public Renderer VideoRenderer = null;
        private Texture2D videoTexture = null;

        private IntPtr spatialCoordinateSystemPtr = IntPtr.Zero;

        private TaskCompletionSource<Wrapper.CaptureState> startPreviewCompletionSource = null;
        private TaskCompletionSource<Wrapper.CaptureState> stopCompletionSource = null;

        private const string startPreview = "start preview";
        private const string stopPreveiw = "stop preview";

#if UNITY_EDITOR

        private void OnGUI()
        {
            int y = 50;
            if (GUI.Button(new Rect(50, y, 200, 50), "Start Preview"))
            {
                StartPreview();
            }

            y += 55;

            if (GUI.Button(new Rect(50, y, 200, 50), "Stop Preview"))
            {
                StopPreview();
            }

            y += 55;

            if (GUI.Button(new Rect(50, y, 200, 50), "Test Transform"))
            {
                UnityEngine.Matrix4x4 testMatrix = UnityEngine.Matrix4x4.TRS(Vector3.forward + Vector3.up * .25f, new Quaternion(0.3826834f, 0.0f, 0.0f, 0.9238796f), Vector3.one);
                UnityEngine.Matrix4x4 testProj = UnityEngine.Matrix4x4.Perspective(64.69f, 1.78f, 0.1f, 1.0f);

                CameraTracker.UpdateCameraMatrices(testMatrix.FromUnity(), testProj.FromUnity());
            }
        }

#endif

        public void OnPreviewButton()
        {
            if (EnabledPreview == false)
            {
                SetSpatialCoordinateSystem();
                StartPreview();
            }
            else
                StopPreview();
        }

        protected override void Awake()
        {
            base.Awake();

            UnityEngine.XR.WSA.WorldManager.OnPositionalLocatorStateChanged += (oldState, newState) =>
            {
                Debug.Log("WorldManager.OnPositionalLocatorStateChanged: " + newState + ", updating any capture in progress");

                SetSpatialCoordinateSystem();
            };
        }

        protected override void OnEnable()
        {
            base.OnEnable();

            if (Application.HasUserAuthorization(UserAuthorization.WebCam))
            {
                Application.RequestUserAuthorization(UserAuthorization.WebCam);
            }

            if (EnableAudio && Application.HasUserAuthorization(UserAuthorization.Microphone))
            {
                Application.RequestUserAuthorization(UserAuthorization.Microphone);
            }

            CreateCapture();

            if (VideoRenderer != null)
            {
                VideoRenderer.enabled = true;
            }
        }

        protected override void OnDisable()
        {
            startPreviewCompletionSource?.TrySetCanceled();
            stopCompletionSource?.TrySetCanceled();

            if (VideoRenderer != null)
            {
                VideoRenderer.material.SetTexture("_MainTex", null);
                VideoRenderer.enabled = false;
            }

            videoTexture = null;

            StopPreview();

            base.OnDisable();
        }

        protected override void OnCallback(Wrapper.CallbackType type, Wrapper.CallbackState args)
        {
            if (type == Wrapper.CallbackType.Capture)
            {
                switch (args.CaptureState.stateType)
                {
                    case Wrapper.CaptureStateType.PreviewStarted:
                        startPreviewCompletionSource?.TrySetResult(args.CaptureState);
                        break;

                    case Wrapper.CaptureStateType.PreviewStopped:
                        stopCompletionSource?.TrySetResult(args.CaptureState);
                        break;

                    case Wrapper.CaptureStateType.PreviewVideoFrame:
                        OnPreviewFrameChanged(args.CaptureState);
                        break;
                }
            }
        }

        protected override void OnFailed(Wrapper.FailedState args)
        {
            base.OnFailed(args);

            startPreviewCompletionSource?.TrySetCanceled();

            stopCompletionSource?.TrySetCanceled();
        }

        protected void OnPreviewFrameChanged(Wrapper.CaptureState state)
        {
            var sizeChanged = false;

            if (videoTexture == null)
            {
                if (state.width != Width || state.height != Height)
                {
                    Debug.Log("Video texture does not match the size requested, using " + state.width + " x " + state.height);
                }

                videoTexture = Texture2D.CreateExternalTexture(state.width, state.height, TextureFormat.BGRA32, false, false, state.imgTexture);

                sizeChanged = true;

                if (VideoRenderer != null)
                {
                    VideoRenderer.enabled = true;
                    VideoRenderer.sharedMaterial.SetTexture("_MainTex", videoTexture);
                    VideoRenderer.sharedMaterial.SetTextureScale("_MainTex", new Vector2(1, -1)); // flip texture
                }
            }
            else if (videoTexture.width != state.width || videoTexture.height != state.height)
            {
                Debug.Log("Video texture size changed, using " + state.width + " x " + state.height);

                videoTexture.UpdateExternalTexture(state.imgTexture);

                sizeChanged = true;
            }

            if (sizeChanged)
            {
                Debug.Log($"Size Changed = {state.width} x {state.height}");

                Width = state.width;
                Height = state.height;
            }

            if (CameraTracker != null)
            {
                CameraTracker.UpdateCameraMatrices(state.cameraWorld, state.cameraProjection);
            }
        }

        private void SetSpatialCoordinateSystem()
        {
            spatialCoordinateSystemPtr = UnityEngine.XR.WSA.WorldManager.GetNativeISpatialCoordinateSystemPtr();
            if (instanceId != Wrapper.InvalidHandle)
            {
                CheckHR(Native.SetCoordinateSystem(instanceId, spatialCoordinateSystemPtr));
            }
        }

        private void CreateCapture()
        {
            IntPtr thisObjectPtr = GCHandle.ToIntPtr(thisObject);
            CheckHR(Wrapper.CreateCapture(stateChangedCallback, thisObjectPtr, out instanceId));
        }

        public async void StartPreview()
        {
            await StartPreviewAsync(Width, Height, EnableAudio, EnableMrc);
        }

        public async void StopPreview()
        {
            if (!await StopPreviewAsync())
            {
                StopPreview();
            }
        }

        public async Task<bool> StartPreviewAsync(int width, int height, bool enableAudio, bool useMrc)
        {
            startPreviewCompletionSource?.TrySetCanceled();

            var hr = Native.StartPreview(instanceId, (UInt32)width, (UInt32)height, enableAudio, useMrc);
            if (hr == 0)
            {
                EnabledPreview = true;
                startPreviewCompletionSource = new TaskCompletionSource<Wrapper.CaptureState>();

                try
                {
                    await startPreviewCompletionSource.Task;
                }
                catch (Exception ex)
                {
                    // task could have been cancelled
                    Debug.LogError(ex.Message);
                    hr = ex.HResult;
                }

                startPreviewCompletionSource = null;
            }
            else
            {
                await Task.Yield();
            }

            return (hr == 0);
        }

        public async Task<bool> StopPreviewAsync()
        {
            stopCompletionSource?.TrySetCanceled();

            var hr = Native.StopPreview(instanceId);
            if (hr == 0)
            {
                EnabledPreview = false;
                stopCompletionSource = new TaskCompletionSource<Wrapper.CaptureState>();

                try
                {
                    var state = await stopCompletionSource.Task;
                }
                catch (Exception ex)
                {
                    Debug.LogError(ex.Message);

                    hr = ex.HResult;
                }
            }
            else
            {
                await Task.Yield();
            }

            stopCompletionSource = null;

            videoTexture = null;

            return CheckHR(hr) == 0;
        }

        private static class Native
        {
            [DllImport(Wrapper.ModuleName, CallingConvention = CallingConvention.StdCall, EntryPoint = "CaptureStartPreview")]
            internal static extern Int32 StartPreview(Int32 handle, UInt32 width, UInt32 height, [MarshalAs(UnmanagedType.I1)] Boolean enableAudio, [MarshalAs(UnmanagedType.I1)] Boolean enableMrc);

            [DllImport(Wrapper.ModuleName, CallingConvention = CallingConvention.StdCall, EntryPoint = "CaptureStopPreview")]
            internal static extern Int32 StopPreview(Int32 handle);

            [DllImport(Wrapper.ModuleName, CallingConvention = CallingConvention.StdCall, EntryPoint = "CaptureSetCoordinateSystem")]
            internal static extern Int32 SetCoordinateSystem(Int32 instanceId, IntPtr spatialCoordinateSystemPtr);
        }
    }
}
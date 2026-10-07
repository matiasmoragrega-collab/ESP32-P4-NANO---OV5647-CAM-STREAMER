import { defineStore } from "pinia";
import { ref } from "vue";
import type { Camera, } from "@/camera";

export const useMainStore = defineStore("main", () => {
  const clientCameras = ref<Camera[]>([]);
  const netRequestError = ref<boolean>(false);
  /** Camera number (index into clientCameras) whose settings panel is open, or null. Only one panel is open at a time. */
  const settingsPanelCamNum = ref<number | null>(null);

  let updateIntervalId: ReturnType<typeof setInterval> | null = null;

  const updateCameraStatus = async () => {
    if (updateIntervalId) clearInterval(updateIntervalId);
    updateIntervalId = null;

    try {
      const response = await fetch("/api/get_camera_info");
      const data: {cameras: Camera[]} = await response.json();
      clientCameras.value.length = data.cameras.length;

      for (const camNum in data.cameras) {
        const camIndex = Number(camNum);
        clientCameras.value[camIndex] = data.cameras[camIndex];
      }
    } catch (e) {
      console.error(e);
      netRequestError.value = true;
    } finally {
      updateIntervalId = setInterval(updateCameraStatus, 3000);
    }
  }

  return {
    clientCameras,
    netRequestError,
    settingsPanelCamNum,
    updateCameraStatus,
  };
});

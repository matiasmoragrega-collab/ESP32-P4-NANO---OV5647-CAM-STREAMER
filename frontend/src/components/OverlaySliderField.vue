<template>
  <v-slider :model-value="modelValue" :min="min" :max="max" :step="step" :disabled="disabled" :label="label"
    hide-details class="overlay-slider-field" @update:model-value="commit($event)">
    <template #append>
      <v-text-field v-model="text" type="number" :min="min" :max="max" :step="step" :suffix="suffix"
        :disabled="disabled" :aria-label="`${label} (${suffix})`" density="compact" variant="outlined" hide-details
        single-line class="overlay-slider-field__input" @focus="focused = true" @blur="onBlur"
        @keydown.enter.prevent="commitText" @keydown.esc="text = format(modelValue)" />
    </template>
  </v-slider>
</template>

<script setup lang="ts">
import { ref } from 'vue'

/** A slider with a numeric field for exact entry. The field commits on Enter / blur (validated and clamped). */
const props = withDefaults(defineProps<{
  modelValue: number,
  min: number,
  max: number,
  step?: number,
  label: string,
  suffix?: string,
  disabled?: boolean,
}>(), {
  step: 0.1,
  suffix: '%',
  disabled: false,
})

const emit = defineEmits<{
  'update:modelValue': [value: number],
}>()

const decimals = () => Math.max(0, (String(props.step).split('.')[1] ?? '').length)
const format = (value: number) => String(Number(value.toFixed(decimals())))

const text = ref<string>(format(props.modelValue))
const focused = ref<boolean>(false)

const commit = (value: number) => {
  if (!Number.isFinite(value)) return props.modelValue
  const factor = 10 ** decimals()
  let clamped = Math.round(Math.min(props.max, Math.max(props.min, value)) * factor) / factor
  if (clamped === 0) clamped = 0 // no -0
  emit('update:modelValue', clamped)
  return clamped
}

const commitText = () => {
  const trimmed = text.value.trim().replace(',', '.')
  const value = Number(trimmed)
  // Show the (possibly clamped) value, or revert invalid input.
  text.value = format(trimmed !== '' && Number.isFinite(value) ? commit(value) : props.modelValue)
}

const onBlur = () => {
  focused.value = false
  commitText()
}

// Keep the field in sync with outside changes (slider, buttons, drag), but don't fight the user while typing.
watch(() => props.modelValue, (value) => {
  if (!focused.value) text.value = format(value)
}, { flush: 'post' })
</script>

<style scoped>
/* Same label width for every row so the sliders line up. */
.overlay-slider-field :deep(.v-slider__label) {
  min-width: 4.2em;
}

.overlay-slider-field__input {
  width: 92px;
  flex: 0 0 auto;
}

/* Hide the browser spinners; the slider and buttons cover coarse changes. */
.overlay-slider-field__input :deep(input[type='number']) {
  -moz-appearance: textfield;
  appearance: textfield;
}

.overlay-slider-field__input :deep(input::-webkit-outer-spin-button),
.overlay-slider-field__input :deep(input::-webkit-inner-spin-button) {
  -webkit-appearance: none;
  margin: 0;
}
</style>

<template>
  <v-slider :model-value="modelValue" :min="min" :max="max" :step="step" :disabled="disabled" :label="label"
    hide-details class="slider-field" :style="{ '--slider-field-label-width': labelWidth }"
    @update:model-value="commit($event)">
    <template #append>
      <v-text-field v-model="text" type="number" :min="min" :max="max" :step="step" :suffix="suffix || undefined"
        :disabled="disabled" :aria-label="suffix ? `${label} (${suffix})` : label" density="compact"
        variant="outlined" hide-details single-line class="slider-field__input" @focus="focused = true"
        @blur="onBlur" @keydown.enter.prevent="commitText" @keydown.esc="text = format(modelValue)" />
    </template>
  </v-slider>
</template>

<script setup lang="ts">
import { ref } from 'vue'

/**
 * A slider with a numeric field for exact entry. The field commits on Enter / blur: the typed value is clamped to
 * [min, max] and snapped to the nearest `min + k * step`; empty / invalid input (or Escape) restores the current value.
 * Precision (integer vs decimal) follows `step` (and `min`).
 */
const props = withDefaults(defineProps<{
  modelValue: number,
  min: number,
  max: number,
  step?: number,
  label: string,
  suffix?: string,
  disabled?: boolean,
  /** Fixed label column width, so the tracks of neighboring rows line up; longer labels wrap. */
  labelWidth?: string,
}>(), {
  step: 1,
  suffix: '',
  disabled: false,
  labelWidth: '4.2em',
})

const emit = defineEmits<{
  'update:modelValue': [value: number],
}>()

const decimalsOf = (value: number) => {
  const [mantissa, exponent] = String(value).toLowerCase().split('e')
  const fraction = (mantissa.split('.')[1] ?? '').length
  return Math.max(0, fraction - Number(exponent ?? 0))
}
const decimals = () => Math.min(10, Math.max(decimalsOf(props.step), decimalsOf(props.min)))
const round = (value: number) => {
  const factor = 10 ** decimals()
  const rounded = Math.round(value * factor) / factor
  return rounded === 0 ? 0 : rounded // no -0
}
const format = (value: number) => String(round(value))

/** Clamps to [min, max] and snaps to the nearest valid step relative to `min` (never past `max`). */
const snap = (value: number) => {
  const { min, max, step } = props
  const clamped = Math.min(max, Math.max(min, value))
  // Work in integer units of the display precision so decimal steps don't pick up float error.
  const factor = 10 ** decimals()
  const stepUnits = Math.round(step * factor)
  if (!(stepUnits > 0)) return round(clamped)
  const minUnits = Math.round(min * factor)
  const maxUnits = max * factor
  let units = minUnits + Math.round((clamped * factor - minUnits) / stepUnits) * stepUnits
  if (units > maxUnits) units = minUnits + Math.floor((maxUnits - minUnits) / stepUnits) * stepUnits
  return round(Math.max(minUnits, units) / factor)
}

const text = ref<string>(format(props.modelValue))
const focused = ref<boolean>(false)

const commit = (value: number) => {
  if (!Number.isFinite(value)) return props.modelValue
  const snapped = snap(value)
  emit('update:modelValue', snapped)
  return snapped
}

const commitText = () => {
  const trimmed = text.value.trim().replace(',', '.')
  const value = Number(trimmed)
  // Show the (possibly clamped / snapped) value, or revert invalid input.
  text.value = format(trimmed !== '' && Number.isFinite(value) ? commit(value) : props.modelValue)
}

const onBlur = () => {
  focused.value = false
  commitText()
}

// Keep the field in sync with outside changes (slider, buttons, drag, reset), but don't fight the user while typing.
watch(() => props.modelValue, (value) => {
  if (!focused.value) text.value = format(value)
}, { flush: 'post' })
</script>

<style scoped>
/* Same label width for every row so the sliders line up; long labels wrap instead of squeezing the track. */
.slider-field :deep(.v-slider__label) {
  min-width: var(--slider-field-label-width);
  max-width: var(--slider-field-label-width);
  white-space: normal;
  line-height: 1.2;
}

.slider-field__input {
  width: 92px;
  flex: 0 0 auto;
}

/* Hide the browser spinners; the slider and buttons cover coarse changes. */
.slider-field__input :deep(input[type='number']) {
  -moz-appearance: textfield;
  appearance: textfield;
}

.slider-field__input :deep(input::-webkit-outer-spin-button),
.slider-field__input :deep(input::-webkit-inner-spin-button) {
  -webkit-appearance: none;
  margin: 0;
}
</style>

<template>
  <div class="dashboard">
    <header class="dashboard-header">
      <h1>{{ title }}</h1>
      <span class="last-updated">{{ formatDate(lastUpdated) }}</span>
    </header>
    <div class="chart-container" v-if="chartData.length > 0">
      <canvas ref="chartCanvas"></canvas>
    </div>
    <div class="controls">
      <button @click="fetchData" :disabled="loading">Refresh</button>
      <select v-model="selectedRange">
        <option value="7d">Last 7 days</option>
        <option value="30d">Last 30 days</option>
        <option value="90d">Last 90 days</option>
      </select>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, watch } from 'vue'

const props = defineProps({
  endpoint: { type: String, required: true },
  title: { type: String, default: 'Analytics Dashboard' }
})

const chartData = ref([])
const loading = ref(false)
const lastUpdated = ref(null)
const selectedRange = ref('30d')
const chartCanvas = ref(null)

const totalValue = computed(() => {
  return chartData.value.reduce((sum, point) => sum + point.value, 0)
})

const averageValue = computed(() => {
  if (chartData.value.length === 0) return 0
  return totalValue.value / chartData.value.length
})

async function fetchData() {
  loading.value = true
  try {
    const response = await fetch(`${props.endpoint}?range=${selectedRange.value}`)
    const json = await response.json()
    chartData.value = json.data || []
    lastUpdated.value = new Date()
  } catch (error) {
    console.error('Failed to fetch dashboard data:', error.message)
    chartData.value = []
  } finally {
    loading.value = false
  }
}

function updateChart() {
  if (!chartCanvas.value || chartData.value.length === 0) return
  const ctx = chartCanvas.value.getContext('2d')
  const width = chartCanvas.value.width
  const height = chartCanvas.value.height
  ctx.clearRect(0, 0, width, height)
  const maxVal = Math.max(...chartData.value.map(d => d.value))
  chartData.value.forEach((point, index) => {
    const x = (index / chartData.value.length) * width
    const barHeight = (point.value / maxVal) * height
    ctx.fillStyle = point.value > averageValue.value ? '#4CAF50' : '#FF9800'
    ctx.fillRect(x, height - barHeight, width / chartData.value.length - 2, barHeight)
  })
}

function formatDate(date) {
  if (!date) return 'Never'
  return date.toLocaleString()
}

watch(chartData, updateChart)
onMounted(fetchData)
</script>

const count = document.getElementById('count');
const button = document.getElementById('increment');
const input = document.getElementById('name');
const status = document.getElementById('status');
const canvas = document.getElementById('plot');
const context = canvas.getContext('2d', { alpha: false });

const state = { clicks: 0, input: '', frames: 0, moduleLoaded: true, promiseDone: false };
window.uiExample = state;

button.addEventListener('click', () => {
  count.textContent = String(++state.clicks);
});
input.addEventListener('input', () => {
  state.input = input.value;
  status.textContent = input.value ? `输入：${input.value}` : '等待输入';
});
Promise.resolve().then(() => {
  state.promiseDone = true;
  document.documentElement.dataset.ready = 'true';
});

// CSS controls the layout; the backing store follows its content box in device
// pixels. Draw in CSS pixels so circles and stroke widths do not stretch.
let plotWidth = canvas.clientWidth;
let plotHeight = canvas.clientHeight;
const plotResizeObserver = new ResizeObserver(entries => {
  plotWidth = entries[0].contentRect.width;
  plotHeight = entries[0].contentRect.height;
});
plotResizeObserver.observe(canvas);

function draw(time) {
  const width = plotWidth;
  const height = plotHeight;
  if (width <= 0 || height <= 0) {
    requestAnimationFrame(draw);
    return;
  }
  const ratio = window.devicePixelRatio;
  const pixelWidth = Math.max(1, Math.round(width * ratio));
  const pixelHeight = Math.max(1, Math.round(height * ratio));
  if (canvas.width !== pixelWidth) canvas.width = pixelWidth;
  if (canvas.height !== pixelHeight) canvas.height = pixelHeight;
  context.setTransform(pixelWidth / width, 0, 0, pixelHeight / height, 0, 0);
  context.fillStyle = '#151d2c';
  context.fillRect(0, 0, width, height);
  context.strokeStyle = '#26334a';
  context.lineWidth = 1;
  for (let y = 35; y < height; y += 35) {
    context.beginPath();
    context.moveTo(22, y);
    context.lineTo(width - 22, y);
    context.stroke();
  }
  context.strokeStyle = '#98afff';
  context.lineWidth = 2.5;
  context.beginPath();
  for (let x = 22; x <= width - 22; x += 2) {
    const y = height / 2 + Math.sin(x / 44 + time / 1100) * 34 + Math.cos(x / 75) * 15;
    if (x === 22) context.moveTo(x, y);
    else context.lineTo(x, y);
  }
  context.stroke();
  context.fillStyle = '#6bd5b0';
  context.beginPath();
  context.arc(width - 40, 49, 4, 0, Math.PI * 2);
  context.fill();
  ++state.frames;
  requestAnimationFrame(draw);
}
requestAnimationFrame(draw);

export { state };

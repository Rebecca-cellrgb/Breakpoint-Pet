const stateNames = {
  idle: "待机",
  confirm_save: "等待确认",
  capture_content: "正在记录",
  away: "用户离开",
};

async function post(url, body = {}) {
  const response = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  if (!response.ok) throw new Error(`请求失败：${response.status}`);
  await refresh();
}

function renderHistory(items) {
  const target = document.querySelector("#history");
  target.innerHTML = items.length
    ? items.map((item) => `
      <div class="message ${item.speaker}">
        <span>${item.speaker === "user" ? "用户" : item.speaker === "pet" ? "萌宠" : "系统"}</span>
        <p>${escapeHtml(item.text)}</p>
      </div>`).join("")
    : '<p class="empty">还没有对话。</p>';
  target.scrollTop = target.scrollHeight;
}

function renderBreakpoints(items) {
  const target = document.querySelector("#breakpoints");
  target.innerHTML = items.length
    ? items.map((item) => `
      <div class="breakpoint ${item.completed_at ? "complete" : "pending"}">
        <div><strong>#${item.id}</strong><span>${item.completed_at ? "已完成" : "未完成"}</span></div>
        <p>${escapeHtml(item.content)}</p>
        <small>${escapeHtml(item.created_at.replace("T", " "))}</small>
      </div>`).join("")
    : '<p class="empty">还没有保存断点。</p>';
}

function escapeHtml(text) {
  const node = document.createElement("div");
  node.textContent = text;
  return node.innerHTML;
}

async function refresh() {
  const response = await fetch("/api/state");
  const data = await response.json();
  const state = document.querySelector("#state");
  state.textContent = stateNames[data.state] || data.state;
  state.dataset.state = data.state;
  document.querySelector("#feedback").textContent =
    `灯板：${data.hardware_feedback.led} · 舵机：${data.hardware_feedback.servo}`;
  renderHistory(data.history);
  renderBreakpoints(data.breakpoints);
}

document.querySelectorAll("[data-endpoint]").forEach((button) => {
  button.addEventListener("click", () => post(button.dataset.endpoint));
});

document.querySelector("#voice-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const input = document.querySelector("#voice-text");
  await post("/api/voice", { text: input.value });
  input.value = "";
  input.focus();
});

document.querySelectorAll("[data-voice]").forEach((button) => {
  button.addEventListener("click", () => post("/api/voice", { text: button.dataset.voice }));
});

document.querySelector("#reset").addEventListener("click", () => post("/api/reset"));

refresh();


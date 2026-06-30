const pathParts = window.location.pathname.split("/");
const appId = pathParts.length >= 3 && pathParts[1] === "lua" ? pathParts[2] : "lua_demo";
const toggle = document.getElementById("toggle");
const statusEl = document.getElementById("status");

async function sendState(enabled) {
  statusEl.textContent = "Sending...";
  const response = await fetch(`/api/lua/${appId}/toggle`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ enabled }),
  });
  const data = await response.json();
  statusEl.textContent = data.enabled ? "Switch is on" : "Switch is off";
}

fetch(`/api/lua/${appId}/state`)
  .then((response) => response.json())
  .then((data) => {
    toggle.checked = !!data.enabled;
    statusEl.textContent = toggle.checked ? "Switch is on" : "Switch is off";
  })
  .catch(() => {
    statusEl.textContent = "State unavailable";
  });

toggle.addEventListener("change", () => {
  sendState(toggle.checked).catch(() => {
    statusEl.textContent = "Request failed";
  });
});

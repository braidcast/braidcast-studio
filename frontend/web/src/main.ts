import { mount } from "svelte";
import "./app.css";
import App from "./App.svelte";
import DetachedApp from "./DetachedApp.svelte";
import { DETACHED_DOCK } from "$lib/utils/windowContext";

// A detached window (index.html?dock=<id>) mounts the single-dock shell; the main
// window (no ?dock=) mounts the full App.
const app = mount(DETACHED_DOCK ? DetachedApp : App, { target: document.getElementById("app")! });

export default app;

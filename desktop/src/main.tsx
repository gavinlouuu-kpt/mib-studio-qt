import React from "react";
import ReactDOM from "react-dom/client";
import App from "./App";
import { AuthGate } from "./components/AuthGate";
import { productName } from "./brand";
import { isRemote } from "./transport";

// The browser UI is served by the instrument's server only, so it is YOFO Studio from the first
// paint (the token prompt included), before the capability report arrives (#550 m13).
if (isRemote) document.title = productName({ pz7035: false, remote: true });

ReactDOM.createRoot(document.getElementById("root") as HTMLElement).render(
  <React.StrictMode>
    <AuthGate>
      <App />
    </AuthGate>
  </React.StrictMode>,
);

import { Component, type ReactNode } from "react";

function PanelContent({ render }: { render: () => ReactNode }) {
  return render();
}

export class PanelErrorBoundary extends Component<{ children: ReactNode | (() => ReactNode) }, { failed: boolean }> {
  state = { failed: false };

  static getDerivedStateFromError() { return { failed: true }; }

  render() {
    if (this.state.failed) return <section role="alert">
      <p>This view could not be displayed. Run controls and the log remain available.</p>
      <button className="btn" onClick={() => this.setState({ failed: false })}>Reload view</button>
    </section>;
    return typeof this.props.children === "function"
      ? <PanelContent render={this.props.children} />
      : this.props.children;
  }
}

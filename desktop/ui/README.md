# @mib/ui — UI language kit

Shared components and tokens for MIB Studio, YOFO Studio and YOFO Review. See `knowledge_map/frontend/UI-Language-Kit.md` for the overview and `guides/ui-language.md` for the rules.

```sh
cd desktop && npm ci            # one install for the desktop app and this package
cd ui && PATH=../node_modules/.bin:$PATH npm run build
```

```tsx
import "@mib/ui/fonts.css";
import "@mib/ui/style.css";
import { AppShell, Card, StateChip } from "@mib/ui";
```

- `docs/<Name>.md`: usage per component, with its Penpot board references (`penpot.json`)
- `.design-sync/`: inputs for syncing the kit to a Claude Design design-system project (config, notes, conventions, authored previews)

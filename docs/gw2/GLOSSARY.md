# The GW2 fork of DXMT

The words this fork uses for its own concepts: the D3D11→Metal driver as GW2 on Mac changes it for
Guild Wars 2. Upstream's `docs/GLOSSARY.md` belongs to upstream; the fork's language lives here.
Each section comes from the architecture council of 2026-11-04 and arrives with the change that
uses it.

## Language

### Switches

**Switch**:
A `DXMT_*` setting, with a fallback and a grammar, read by the module that declares it.
_Avoid_: flag, option, env var

**Requested value**:
What the environment or the control file asks a switch for, as raw text.

**Effective value**:
What the process actually uses after parsing, fallbacks and capability checks; the startup line
shows it.

**Live switch**:
A switch the control file may change while the game runs.
_Avoid_: hot reload

**Control file**:
The file named by `DXMT_CONTROL` that carries live overrides.

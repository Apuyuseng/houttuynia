//! mwine-wine：Windows exe 在 macOS 上安装/运行的 Wine 子系统。
//!
//! - [`runtime::WineRuntime`]：Wine 运行时发现（MWINE_WINE > /Applications >
//!   Caskroom > PATH）与统一命令环境（禁 Mono/Gecko/winemenubuilder）
//! - [`prefix::WineStore`]：精简模板前缀 + APFS clonefile 克隆 + 裁剪 +
//!   应用索引（`mwine wine install/run/list/trim/uninstall/kill` 的底层）
//!
//! 前缀目录布局（默认 `~/Library/Application Support/mwine/wine/`）：
//! ```text
//! template/     精简模板（禁 Mono/Gecko 一次性创建）
//! apps/<名>/    每应用前缀（模板写时复制克隆）
//! index.json    应用索引（安装器来源、发现的主程序、运行统计）
//! ```

pub mod prefix;
pub mod runtime;

pub use prefix::{civil_from_days, dir_size, fmt_ts, human_bytes, sanitize_name, AppEntry, ExeInfo, TrimReport, WineStore};
pub use runtime::{WineRuntime, DLL_OVERRIDES};

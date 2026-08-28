# -*- coding: utf-8 -*-
"""
bmpclient/virtual_media_config.py — VirtualMedia 配置加载。

配置文件为 JSON，默认路径 bmpclient/config/virtual_media.json。
合并重构后（docs/07）配置项覆盖打散策略与段聚合写路径：
strategy / step_idx / step_layer / prime / super_page_bytes /
super_page_bytes_per_device / flush_policy / max_topk。
文件不存在时使用默认配置（position_hash + 默认参数）。
"""

import json
import os
from typing import Any, Dict, Optional

DEFAULT_STRATEGY = "position_hash"
DEFAULT_SUPER_PAGE_BYTES = 2 * 1024 * 1024


def default_config() -> Dict[str, Any]:
    """返回默认配置。"""
    return {
        "strategy": DEFAULT_STRATEGY,
        "step_idx": 17,
        "step_layer": 23,
        "prime": 2147483647,
        "super_page_bytes": DEFAULT_SUPER_PAGE_BYTES,
        "flush_policy": "on_request_end",
        "max_topk": 512,
    }


def default_config_path() -> str:
    """返回默认配置文件路径：bmpclient/config/virtual_media.json。"""
    package_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(package_dir, "config", "virtual_media.json")


def load_config(path: Optional[str] = None) -> Dict[str, Any]:
    """
    加载 VirtualMedia 配置文件。

    :param path: 配置文件路径，None 则使用默认路径
    :return: 配置字典（默认值 + 文件覆盖）
    """
    path = path or default_config_path()

    cfg = default_config()
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            user_cfg = json.load(f)
        if not isinstance(user_cfg, dict):
            raise ValueError(f"config file {path} must contain a JSON object")
        cfg.update(user_cfg)

    return cfg


def get_strategy_name(config: Dict[str, Any]) -> str:
    """从配置中提取策略名。"""
    return str(config.get("strategy", DEFAULT_STRATEGY)).lower()


def get_strategy_options(config: Dict[str, Any]) -> Dict[str, Any]:
    """
    返回传递给策略构造函数的选项字典。

    排除顶级 strategy 与介质层字段，保留 step_idx / step_layer / prime /
    max_token_idx / max_layer_id 等策略参数。
    """
    excluded = {
        "strategy", "super_page_bytes", "super_page_bytes_per_device",
        "flush_policy", "max_topk",
    }
    return {k: v for k, v in config.items() if k not in excluded}

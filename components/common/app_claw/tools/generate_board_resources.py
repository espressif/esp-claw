#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Generate hardware ownership data from standard board-manager YAML fields."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import json
from pathlib import Path
import re
import sys

import yaml


class BoardResourceError(ValueError):
    pass


def mapping(value, where):
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise BoardResourceError(f'{where}: expected a mapping')
    return value


def sequence(value, where):
    if value is None:
        return []
    if not isinstance(value, list):
        raise BoardResourceError(f'{where}: expected a list')
    return value


def number(value, family, where):
    """Accept literals and known SDK identifiers, never arbitrary C expressions."""
    if isinstance(value, bool):
        raise BoardResourceError(f'{where}: boolean is not a resource number')
    if isinstance(value, int):
        return value
    if not isinstance(value, str):
        raise BoardResourceError(f'{where}: invalid resource number {value!r}')
    value = value.strip()
    if re.fullmatch(r'-?(?:0[xX][0-9a-fA-F]+|[0-9]+)', value):
        return int(value, 16 if 'x' in value.lower() else 10)
    aliases = {
        'gpio': (r'GPIO_NUM_(\d+)', 0), 'i2c': (r'I2C_NUM_(\d+)', 0),
        'i2s': (r'I2S_NUM_(\d+)', 0), 'spi': (r'SPI([123])_HOST', -1),
        'uart': (r'UART_NUM_(\d+)', 0), 'adc': (r'ADC_UNIT_([12])', -1),
        'adc_channel': (r'ADC_CHANNEL_(\d+)', 0), 'ledc_timer': (r'LEDC_TIMER_(\d+)', 0),
        'ledc_channel': (r'LEDC_CHANNEL_(\d+)', 0), 'sdmmc': (r'SDMMC_HOST_SLOT_([01])', 0),
    }
    if family == 'gpio' and value in ('GPIO_NUM_NC', 'GPIO_NUM_NC_WITH_PULLUP', 'I2S_GPIO_UNUSED', 'UART_PIN_NO_CHANGE'):
        return -1
    if family == 'ledc_mode' and value in ('LEDC_LOW_SPEED_MODE', 'LEDC_HIGH_SPEED_MODE'):
        return value
    if family in aliases:
        pattern, offset = aliases[family]
        match = re.fullmatch(pattern, value)
        if match:
            return int(match[1]) + offset
    raise BoardResourceError(f'{where}: unsupported {family} expression {value!r}')


@dataclass(frozen=True)
class Resource:
    kind: str
    a: int | str
    b: int | str = 0


@dataclass
class BoardResources:
    resources: list[Resource] = field(default_factory=list)
    devices: list[tuple[str, str]] = field(default_factory=list)
    ignored_custom: list[str] = field(default_factory=list)
    groups: list = field(default_factory=list)
    deferred_peripherals: list[str] = field(default_factory=list)

    def add(self, kind, a, b=0):
        resource = Resource(kind, a, b)
        if resource not in self.resources:
            self.resources.append(resource)

    def gpio(self, value, where):
        pin = number(value, 'gpio', where)
        if pin < -1 or pin > 255:
            raise BoardResourceError(f'{where}: invalid GPIO {pin}')
        if pin >= 0:
            self.add('gpio', pin)

    def pins(self, config, fields, where):
        for key in fields:
            if key in config:
                self.gpio(config[key], f'{where}.{key}')


def load_items(path, key):
    document = mapping(yaml.safe_load(path.read_text()), str(path))
    if key not in document or set(document) - {key, 'version'}:
        raise BoardResourceError(f'{path}: expected only {key!r}; merge included/amended configuration first')
    items = sequence(document[key], f'{path}:{key}')
    if document[key] is None:
        raise BoardResourceError(f'{path}:{key}: use an explicit empty list')
    names = set()
    result = {}
    for raw in items:
        item = mapping(raw, str(path))
        name = item.get('name')
        if not isinstance(name, str) or not name or len(name.encode()) > 39:
            raise BoardResourceError(f'{path}: invalid or overlong resource name {name!r}')
        for flag in ('gen_skip', 'init_skip'):
            if flag in item and not isinstance(item[flag], bool):
                raise BoardResourceError(f'{path}:{name}: {flag} must be boolean')
        if not item.get('gen_skip', False):
            if name in names:
                raise BoardResourceError(f'{path}: duplicate name {name}')
            names.add(name)
            result[name] = item
    return result


def parse_peripheral(out, item):
    name = item['name']
    kind = item.get('type')
    config = mapping(item.get('config') or (item if kind in ('spi', 'ledc') else None), name)
    pins = mapping(config.get('pins'), f'{name}.pins')
    if kind == 'gpio':
        if 'pin' not in config:
            raise BoardResourceError(f'{name}: GPIO pin is required')
        out.gpio(config['pin'], name)
    elif kind in ('i2c', 'i2s'):
        default = -1 if kind == 'i2c' else 0
        port = number(config.get('port', default), kind, f'{name}.port')
        if port < 0:
            raise BoardResourceError(f'{name}: auto-selected {kind} port cannot be reserved statically')
        out.add(f'{kind}_bus', port)
        out.pins(pins, ('sda', 'scl') if kind == 'i2c' else ('mclk', 'bclk', 'ws', 'dout', 'dout2', 'din', 'clk', 'din0', 'din1', 'din2', 'din3'), name)
    elif kind == 'spi':
        bus = mapping(config.get('spi_bus_config'), name)
        host = number(bus.get('spi_port', config.get('spi_port', 0)), 'spi', f'{name}.spi_port')
        if host < 0:
            raise BoardResourceError(f'{name}: invalid SPI host')
        out.add('spi_bus', host)
        out.pins(bus, ('sclk_io_num', 'mosi_io_num', 'miso_io_num', 'quadwp_io_num', 'quadhd_io_num', *(f'data{i}_io_num' for i in range(8))), name)
    elif kind == 'uart':
        out.add('uart_bus', number(config.get('uart_num'), 'uart', name))
        out.pins(config, ('tx_io_num', 'rx_io_num', 'rts_io_num', 'cts_io_num'), name)
    elif kind == 'ledc':
        out.gpio(config.get('gpio_num', -1), name)
        mode = number(config.get('speed_mode', 'LEDC_LOW_SPEED_MODE'), 'ledc_mode', name)
        out.add('ledc_timer', mode, number(config.get('timer_sel', 0), 'ledc_timer', name))
        out.add('ledc_channel', mode, number(config.get('channel', 0), 'ledc_channel', name))
    elif kind in ('rmt', 'sdm'):
        out.pins(config, ('gpio_num',), name)
    elif kind == 'dsi':
        out.add('dsi_bus', number(config.get('bus_id', 0), 'number', name))
    elif kind == 'ldo':
        out.add('ldo', number(config.get('chan_id', 3), 'number', name))
    else:
        raise BoardResourceError(f'{name}: unsupported standard peripheral type {kind!r}')


def peripheral_refs(item):
    config = mapping(item.get('config'), item['name'])
    refs = item.get('peripherals') or config.get('peripherals')
    if not refs and item.get('type') == 'display_lcd':
        refs = mapping(config.get('io_spi_config'), item['name']).get('peripherals')
    return sequence(refs, item['name'])


def i2c_addresses(out, item, peripherals):
    kind = item['type']
    for reference in peripheral_refs(item):
        reference = mapping(reference, item['name'])
        name = reference.get('name')
        if name not in peripherals:
            raise BoardResourceError(f"{item['name']}: unknown or skipped peripheral {name!r}")
        peripheral = peripherals[name]
        if peripheral['type'] == 'i2s' and kind == 'audio_codec':
            out.pins(reference, ('tx_aux_out_io',), item['name'])
        if peripheral['type'] != 'i2c':
            continue
        port = number(mapping(peripheral.get('config'), name).get('port', -1), 'i2c', name)
        if kind == 'camera':
            # SCCB probes addresses dynamically.
            continue
        if kind == 'audio_codec':
            values = reference.get('address', 0x30)
        elif kind in ('lcd_touch', 'gpio_expander'):
            if 'i2c_addr' not in reference:
                raise BoardResourceError(f"{item['name']}: i2c_addr is required")
            values = reference['i2c_addr']
        else:
            continue
        if isinstance(values, str) and ',' in values:
            values = [v.strip() for v in values.split(',')]
        valid_addresses = 0
        for raw in values if isinstance(values, list) else [values]:
            address = number(raw, 'number', item['name'])
            if (address == 0 and kind == 'audio_codec') or (address <= 0 and kind == 'lcd_touch'):
                continue
            if not 0 < address <= 0xfe or address & 1:
                raise BoardResourceError(f"{item['name']}: {address:#x} is not a shifted 8-bit I2C address")
            out.add('i2c_addr', port, address >> 1)
            valid_addresses += 1
        if kind == 'lcd_touch' and not valid_addresses:
            raise BoardResourceError(f"{item['name']}: at least one I2C address is required")


def parse_device(out, item, peripherals):
    name, kind = item['name'], item.get('type')
    if kind == 'custom':
        if item.get('chip') not in ('bmi270', 'icm42670', 'mpu6050', 'bmm150', 'bmm350', 'qmc6309'):
            out.ignored_custom.append(name)
            return
        config = mapping(item.get('config'), name)
        refs = peripheral_refs(item)
        if len(refs) != 1:
            raise BoardResourceError(f'{name}: Sensor requires one I2C peripheral')
        peripheral = peripherals.get(mapping(refs[0], name).get('name'), {})
        if peripheral.get('type') != 'i2c':
            raise BoardResourceError(f'{name}: Sensor requires an I2C peripheral')
        port = number(mapping(peripheral.get('config'), name).get('port', -1), 'i2c', name)
        default_address = {'bmm150': 0x10, 'bmm350': 0x14, 'qmc6309': 0x7c}.get(item.get('chip'), 0x68)
        address = number(config.get('i2c_addr', config.get('addr', default_address)), 'number', name)
        # BMM150 addr accepts 7-bit addresses or shifted 8-bit write addresses.
        if item.get('chip') == 'bmm150' and 'i2c_addr' not in config and address in (0x20, 0x22, 0x24, 0x26):
            address >>= 1
        if port < 0 or not 0 <= address <= 0x7f:
            raise BoardResourceError(f'{name}: invalid sensor port or 7-bit address')
        out.add('i2c_addr', port, address)
        out.pins(config, ('int_gpio_num', 'sdo_gpio_num'), name)
        out.devices.append((name, kind))
        return
    config = mapping(item.get('config'), name)
    subtype = item.get('sub_type', '')
    known = {'audio_codec', 'display_lcd', 'lcd_touch', 'fs_fat', 'button', 'camera', 'led_strip', 'gpio_expander', 'gpio_ctrl', 'power_ctrl', 'ledc_ctrl'}
    if kind not in known:
        raise BoardResourceError(f'{name}: unsupported standard device type {kind!r}')
    if kind == 'audio_codec':
        adc = mapping(config.get('adc_local_cfg'), name)
        channels = adc.get('channel_list') or []
        channels = channels if isinstance(channels, list) else [channels]
        patterns = sequence(adc.get('patterns'), name)
        if channels and patterns:
            raise BoardResourceError(f'{name}: ADC channel_list and patterns are mutually exclusive')
        entries = patterns or [{'unit': adc.get('unit_id', 'ADC_UNIT_1'), 'channel': channel} for channel in channels]
        for entry in entries:
            entry = mapping(entry, name)
            unit = number(entry.get('unit', 'ADC_UNIT_1'), 'adc', name)
            channel = number(entry.get('channel', -1), 'adc_channel', name)
            if unit not in (0, 1) or channel < 0:
                raise BoardResourceError(f'{name}: invalid ADC unit/channel')
            out.add('adc_unit', unit)
            out.add('adc_channel', unit, channel)
    i2c_addresses(out, item, peripherals)
    if kind == 'display_lcd':
        if subtype not in ('spi', 'rgb', 'rgb_3wire_spi', 'dsi', 'parlio', 'i80'):
            raise BoardResourceError(f'{name}: unsupported LCD subtype {subtype!r}')
        out.pins(config, ('reset_gpio_num',), name)
        line = mapping(mapping(config.get('io_3wire_spi_config'), name).get('line_config'), name)
        for signal in ('cs', 'scl', 'sda'):
            io_type = line.get(f'{signal}_io_type', 'IO_TYPE_GPIO')
            if io_type == 'IO_TYPE_GPIO':
                out.gpio(line.get(f'{signal}_gpio_num', -1), name)
            elif io_type != 'IO_TYPE_EXPANDER':
                raise BoardResourceError(f'{name}: unknown 3-wire IO type {io_type!r}')
        for section, fields in {
            'io_spi_config': ('cs_gpio_num', 'dc_gpio_num'),
            'lcd_panel_config': ('reset_gpio_num',),
            'rgb_panel_config': ('hsync_gpio_num', 'vsync_gpio_num', 'de_gpio_num', 'pclk_gpio_num', 'disp_gpio_num'),
            'io_parl_config': ('dc_gpio_num', 'clk_gpio_num', 'cs_gpio_num'),
            'i80_bus_config': ('dc_gpio_num', 'wr_gpio_num'),
            'io_i80_config': ('cs_gpio_num',),
        }.items():
            cfg = mapping(config.get(section), name)
            out.pins(cfg, fields, name)
            data_pins = cfg.get('data_gpio_nums')
            if data_pins is None and section in ('rgb_panel_config', 'io_parl_config'):
                width = number(cfg.get('data_width', 16 if section == 'rgb_panel_config' else 8), 'number', name)
                if not 1 <= width <= 32:
                    raise BoardResourceError(f'{name}: invalid data_width {width}')
                data_pins = [cfg.get(f'data_gpio_{i}', -1) for i in range(width)]
            for pin in sequence(data_pins, name):
                out.gpio(pin, name)
    elif kind == 'lcd_touch':
        if subtype not in ('i2c', 'spi'):
            raise BoardResourceError(f'{name}: unsupported touch subtype {subtype!r}')
        out.pins(mapping(config.get('touch_config'), name), ('int_gpio_num', 'rst_gpio_num'), name)
        out.pins(mapping(config.get('io_spi_config'), name), ('cs_gpio_num', 'dc_gpio_num'), name)
    elif kind == 'fs_fat':
        cfg = mapping(config.get('sub_config'), name)
        if subtype == 'sdmmc':
            width = number(cfg.get('bus_width', 1), 'number', name)
            if width not in (1, 4, 8):
                raise BoardResourceError(f'{name}: invalid SDMMC bus_width {width}')
            pins = mapping(cfg.get('pins'), name)
            required = ('clk', 'cmd', 'd0') + tuple(f'd{i}' for i in range(1, width))
            if any(number(pins.get(pin, -1), 'gpio', name) < 0 for pin in required):
                raise BoardResourceError(f'{name}: explicit SDMMC bus pins are required')
            out.add('sdmmc_slot', number(cfg.get('slot', 'SDMMC_HOST_SLOT_1'), 'sdmmc', name), width)
            out.pins(mapping(cfg.get('pins'), name), ('clk', 'cmd', 'cd', 'wp', *(f'd{i}' for i in range(8))), name)
            if cfg.get('ldo_chan_id', -1) != -1:
                out.add('ldo', number(cfg['ldo_chan_id'], 'number', name))
        elif subtype == 'spi':
            out.gpio(cfg.get('cs_gpio_num', 15), name)
            out.pins(cfg, ('cd_gpio_num', 'wp_gpio_num'), name)
        else:
            raise BoardResourceError(f'{name}: unsupported filesystem subtype {subtype!r}')
    elif kind == 'camera':
        if subtype not in ('dvp', 'csi', 'spi'):
            raise BoardResourceError(f'{name}: unsupported camera subtype {subtype!r}')
        cfg = mapping(config.get(subtype + '_config'), name)
        if subtype == 'dvp':
            out.pins(cfg, ('reset_io', 'pwdn_io', 'vsync_io', 'de_io', 'pclk_io', 'xclk_io'), name)
            data = mapping(cfg.get('data_io'), name)
            out.pins(data, tuple(f'data_io_{i}' for i in range(16)), name)
        elif subtype == 'csi':
            out.pins(cfg, ('reset_io', 'pwdn_io'), name)
            clock = mapping(cfg.get('xclk_config'), name)
            out.pins(clock, ('xclk_pin',), name)
            out.pins(mapping(clock.get('esp_clock_router_cfg'), name), ('xclk_pin',), name)
        else:
            out.add('spi_bus', number(cfg.get('spi_port', 0), 'spi', name))
            out.pins(cfg, ('spi_cs_pin', 'spi_sclk_pin', 'spi_data0_io_pin', 'spi_data1_io_pin', 'reset_pin', 'pwdn_pin', 'xclk_pin'), name)
            if cfg.get('xclk_pin', -1) != -1 and cfg.get('xclk_source', 'ESP_CAM_SENSOR_XCLK_LEDC') == 'ESP_CAM_SENSOR_XCLK_LEDC':
                clock = mapping(cfg.get('xclk_ledc_cfg'), name)
                out.add('ledc_timer', 'LEDC_LOW_SPEED_MODE', number(clock.get('timer', 0), 'ledc_timer', name))
                out.add('ledc_channel', 'LEDC_LOW_SPEED_MODE', number(clock.get('channel', 0), 'ledc_channel', name))
    elif kind == 'led_strip':
        out.pins(config, ('strip_gpio_num',), name)
        if subtype == 'spi':
            cfg = mapping(config.get('spi', config.get('spi_config')), name)
            out.add('spi_bus', number(cfg.get('spi_bus', 'SPI2_HOST'), 'spi', name))
        elif subtype != 'rmt':
            raise BoardResourceError(f'{name}: unsupported LED strip subtype {subtype!r}')
    elif kind == 'button' and subtype not in ('gpio', 'adc_single', 'adc_multi'):
        raise BoardResourceError(f'{name}: unsupported button subtype {subtype!r}')
    out.devices.append((name, kind))


def parse_board(board_dir):
    board_dir = Path(board_dir)
    peripherals = load_items(board_dir / 'board_peripherals.yaml', 'peripherals')
    devices = load_items(board_dir / 'board_devices.yaml', 'devices')
    board = BoardResources()
    skipped, active = set(), set()
    for item in devices.values():
        refs = skipped if item.get('init_skip', False) else active
        refs.update(mapping(ref, item['name']).get('name') for ref in peripheral_refs(item))
    # Keep shared and standalone peripherals in the startup initialization list.
    board.deferred_peripherals = sorted(peripherals.keys() & (skipped - active))
    aliases = {}
    for is_device, items in ((False, peripherals), (True, devices)):
        for item in items.values():
            part = BoardResources()
            if is_device:
                parse_device(part, item, peripherals)
            else:
                parse_peripheral(part, item)
            board.ignored_custom.extend(part.ignored_custom)
            if part.ignored_custom:
                continue
            # Only the codec and I2S driver explicitly support these aliases.
            alias = None
            if item['type'] == 'i2s':
                alias = ('i2s', json.dumps(item.get('config'), sort_keys=True))
            elif item['type'] == 'audio_codec' and part.resources:
                alias = ('codec', item.get('chip'), tuple(part.resources))
            group = aliases.get(alias, len(board.groups)) if alias else len(board.groups)
            if alias:
                aliases[alias] = group
            board.groups.append((item['name'], is_device, group, part))
            board.devices.extend(part.devices)
    return board


def render(board):
    lines = ['/* Generated from board YAML. Do not edit. */', '#include "app_claw_hw_bridge.h"', '#include "driver/ledc.h"']
    rows = []
    for index, (name, device, group, part) in enumerate(board.groups):
        if group == index and part.resources:
            lines.append(f'static const app_claw_hw_resource_t resources_{index}[] = {{')
            for r in part.resources:
                kind = ('ldo_channel' if r.kind == 'ldo' else r.kind).upper()
                lines.append(f'    {{APP_CLAW_HW_{kind}, {r.a}, {r.b}}},')
            lines.append('};')
        resources = f'resources_{group}' if part.resources else 'NULL'
        board_owned = device and any(kind in ('display_lcd', 'lcd_touch') for _, kind in part.devices)
        rows.append(f'    {{{json.dumps(name)}, {str(device).lower()}, {group}, {resources}, {len(part.resources)}, {str(board_owned).lower()}}},')
    if rows:
        lines.extend(['static const app_claw_hw_group_t groups[] = {', *rows, '};'])
    lines.append(f'const app_claw_hw_board_config_t app_claw_hw_generated_board = {{{"groups" if rows else "NULL"}, {len(rows)}}};')
    return '\n'.join(lines) + '\n'


def adapt_manager(source_dir, output_dir, deferred_peripherals=()):
    # Compile local copies with hooks at the actual driver calls; keep vendor files intact.
    for kind in ('periph', 'device'):
        filename = f'esp_board_{kind}.c'
        source = (source_dir / 'src' / filename).read_text()
        device = str(kind == 'device').lower()
        marker = '#include "esp_board_' + kind + '.h"'
        declarations = '\nextern esp_err_t app_claw_hw_driver_init(const char *, bool, esp_err_t (*)(void *, int, void **), void *, int, void **);\nextern esp_err_t app_claw_hw_driver_deinit(const char *, bool, esp_err_t (*)(void *), void *);'
        if marker not in source:
            raise BoardResourceError(f'{filename}: missing board header')
        source = source.replace(marker, marker + declarations, 1)
        replacements = {
            'handle->init((void *)desc->cfg, desc->cfg_size, &list->periph_handle)': f'app_claw_hw_driver_init(name, {device}, handle->init, (void *)desc->cfg, desc->cfg_size, &list->periph_handle)',
            'handle->init((void *)cfg, cfg_size, &handle->device_handle)': f'app_claw_hw_driver_init(name, {device}, handle->init, (void *)cfg, cfg_size, &handle->device_handle)',
            'handle->deinit(list->periph_handle)': f'app_claw_hw_driver_deinit(name, {device}, handle->deinit, list->periph_handle)',
            'handle->deinit(handle->device_handle)': f'app_claw_hw_driver_deinit(name, {device}, handle->deinit, handle->device_handle)',
        }
        count = 0
        for old, new in replacements.items():
            count += source.count(old)
            source = source.replace(old, new)
        if count != (3 if kind == 'periph' else 2):
            raise BoardResourceError(f'{filename}: driver call sites changed; review hardware hooks')
        # Board Manager must retain its reference when peripheral close fails.
        if kind == 'periph':
            source = source.replace('        ESP_BOARD_RETURN_ON_ERROR(ret, TAG, "Failed to deinit periph: %s", name);', '        if (ret != ESP_OK) list->ref_count++;\n        ESP_BOARD_RETURN_ON_ERROR(ret, TAG, "Failed to deinit periph: %s", name);')
            if deferred_peripherals:
                marker = '        ret = esp_board_periph_init(desc->name);'
                if source.count(marker) != 1:
                    raise BoardResourceError(f'{filename}: startup call site changed; review deferred initialization')
                condition = ' || '.join(f'strcmp(desc->name, {json.dumps(name)}) == 0' for name in deferred_peripherals)
                guard = ('        /* Initialize peripherals of skipped devices only on explicit request. */\n'
                         f'        if ({condition}) {{\n'
                         '            desc = desc->next;\n'
                         '            continue;\n'
                         '        }\n')
                source = source.replace(marker, guard + marker, 1)
        target = output_dir / filename
        if not target.exists() or target.read_text() != source:
            target.write_text(source)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--board-dir', type=Path)
    source.add_argument('--empty', action='store_true')
    parser.add_argument('--manager-dir', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        board = BoardResources() if args.empty else parse_board(args.board_dir)
        content = render(board)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        if not args.output.exists() or args.output.read_text() != content:
            temporary = args.output.with_suffix(args.output.suffix + '.tmp')
            temporary.write_text(content)
            temporary.replace(args.output)
        if args.manager_dir:
            adapt_manager(args.manager_dir, args.output.parent, board.deferred_peripherals)
        if board.ignored_custom:
            print('Hardware resources: custom devices ignored: ' + ', '.join(board.ignored_custom), file=sys.stderr)
    except (BoardResourceError, OSError, yaml.YAMLError) as error:
        print(f'Hardware resource generation failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

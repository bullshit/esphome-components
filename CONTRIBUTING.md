# Contributing to ESPHome Components

Thank you for your interest in contributing to this project! This guide will help you get started.

## Reporting Issues

### New UPS Protocol Support

If you have a UPS device that isn't currently supported, please use our **New UPS Protocol Request** issue template. This template guides you through collecting all necessary debug information.

### Protocol Implementation Bugs

For issues with existing protocol support, use the **Protocol Implementation Bug Report** template. This helps us quickly identify and fix problems with supported devices.

#### Quick Start: Bug Debug Collection

For existing protocol issues, use our debug tool:

```bash
# Collect debug data from running ESP32
./tools/debug-protocol-issue.sh 192.168.1.100 my_ups

# Follow interactive prompts to test different states
# Tool will create a complete bug report
```

#### Quick Start: New Protocol Debug Collection

We provide a tool to automatically collect all required debug information:

```bash
# 1. Configure your UPS in /etc/nut/ups.conf
sudo nano /etc/nut/ups.conf
# Add:
# [myups]
#     driver = usbhid-ups
#     port = auto

# 2. Run the collection script
./tools/collect-protocol-debug.sh myups

# 3. Follow the interactive prompts
# 4. Review the generated debug output
# 5. Create an issue with the collected data
```

The script will:
- Detect your UPS USB IDs
- Capture HID communication during different power states
- Extract all UPS variables and supported commands
- Generate a summary report for your issue

#### Manual Debug Collection

If you prefer manual collection, the issue template provides detailed instructions for:
- Capturing NUT driver debug output
- Documenting state transitions (AC → Battery → Charging)
- Identifying HID report structures
- Mapping UPS variables

### Required Information for Protocol Requests

**Essential data needed:**
1. **USB IDs**: Vendor and Product IDs from `lsusb`
2. **HID Reports**: Debug output showing report IDs and data during:
   - Normal operation (on AC power)
   - Battery operation (unplugged)
   - Charging state (plugged back in)
3. **UPS Variables**: Complete `upsc` output
4. **HID Paths**: All discovered HID paths from NUT debug

**Why this matters**: This debug data allows developers (including AI assistants) to:
- Understand the HID report structure
- Map data bytes to UPS variables
- Identify status bits and their meanings
- Implement accurate protocol support

## Development Setup

### Prerequisites

- ESP32-S3 with USB OTG support
- ESPHome development environment
- VS Code with devcontainer support (recommended)
- UPS device for testing

### Quick Start

1. Clone the repository
2. Open in VS Code with devcontainer
3. Test with: `esphome compile configs/testing/simulation_test.yaml`

## Code Contributions

### Adding New Protocols

1. **Study existing protocols**: Review `protocol_apc.cpp`, `protocol_cyberpower.cpp`, or `protocol_tripplite.cpp`
2. **Use the protocol base class**: Inherit from `UpsProtocolBase`
3. **Register your protocol**: Use the `REGISTER_UPS_PROTOCOL_FOR_VENDOR` macro
4. **Follow patterns**: Match existing code style and error handling
5. **Add constants properly**: Use `ups_constants.h` for shared values

### Code Quality Standards

- **No exceptions**: ESPHome disables C++ exceptions
- **Thread safety**: Use mutexes for shared data access
- **Logging**: Use appropriate ESP_LOG levels
- **Constants**: No magic numbers - use named constants
- **Testing**: Provide simulation mode for testing without hardware

### Testing Your Changes

1. **Compile test**: `esphome compile configs/testing/simulation_test.yaml`
2. **Upload to device**: Use VS Code task or `esphome upload`
3. **Monitor logs**: `esphome logs configs/testing/simulation_test.yaml`
4. **Test all states**: Verify online, battery, and charging states
5. **Check memory**: Monitor heap usage and task stack sizes

## Documentation

### Component Documentation

Update the component README (`components/ups_hid/README.md`) when:
- Adding new features
- Changing configuration options
- Adding protocol support
- Fixing significant bugs

### Code Comments

- Document complex algorithms
- Explain protocol-specific quirks
- Reference NUT source files when applicable
- NO unnecessary comments for obvious code

## Pull Request Process

1. **Create focused PRs**: One feature/fix per PR
2. **Test thoroughly**: Include test results in PR description
3. **Update documentation**: README and configuration examples
4. **Follow commit conventions**: Clear, descriptive commit messages
5. **Be responsive**: Address review feedback promptly

## Getting Help

- **GitHub Issues**: For bugs and feature requests
- **Discussions**: For questions and general help
- **Debug logs**: Always include when reporting issues

## License

By contributing, you agree that your contributions will be licensed under the same license as the project.
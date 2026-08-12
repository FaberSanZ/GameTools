#pragma once
/*
    GameInputSystem.h
    -----------------
    Header-only action-based input system for Microsoft GameInput + EnTT.

    Requirements:
      - C++20
      - Microsoft.GameInput NuGet package
      - EnTT (only needed for InstallInRegistry / GetFromRegistry helpers)
      - Link against gameinput.lib

    Design:
      GameInput -> physical device snapshot -> bindings -> actions -> gameplay

    Notes:
      - This version tracks keyboard, mouse, and one active gamepad.
      - Action lookup uses stable handles instead of raw pointers.
      - Maps exposed from InputSystem are const to avoid invalidating lookup data.

    Example:
      entt::registry registry;
      auto& input = game::input::InputSystem::InstallInRegistry(registry);

      using namespace game::input;
      constexpr ActionId Move = Hash("Player/Move");
      constexpr ActionId Jump = Hash("Player/Jump");
      constexpr MapId PlayerMap = Hash("Player");

      InputActionMap player{PlayerMap, "Player"};

      InputAction move{Move, "Move", ActionType::Axis2D};
      move.AddBinding(Binding::Composite2D(
          Control::Key('W'), Control::Key('S'),
          Control::Key('A'), Control::Key('D')));
      move.AddBinding(Binding::Stick2D(
          Control::GamepadAxis(GamepadAxis::LeftX),
          Control::GamepadAxis(GamepadAxis::LeftY),
          0.15f, true));
      player.AddAction(std::move(move));

      InputAction jump{Jump, "Jump", ActionType::Button};
      jump.AddBinding(Binding::Button(Control::Key(VK_SPACE)));
      jump.AddBinding(Binding::Button(
          Control::GamepadButton(GameInputGamepadA)));
      player.AddAction(std::move(jump));

      input.AddMap(std::move(player));

      // Every frame:
      input.Update(deltaTime);

      const Vec2 direction = input.ReadVector2(Move);
      if (input.WasPressedThisFrame(Jump)) { ... }
*/

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <gameinput.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#if __has_include(<entt/entt.hpp>)
#include <entt/entt.hpp>
#define GAME_INPUT_SYSTEM_HAS_ENTT 1
#else
#define GAME_INPUT_SYSTEM_HAS_ENTT 0
#endif

#pragma comment(lib, "gameinput.lib")

namespace game::input
{
    //using namespace GameInput::v3;

    using ActionId = std::uint32_t;
    using MapId = std::uint32_t;

    [[nodiscard]]
    consteval std::uint32_t Hash(const char* text) noexcept
    {
        std::uint32_t hash = 2166136261u;

        while (*text != '\0')
        {
            hash ^= static_cast<std::uint8_t>(*text++);
            hash *= 16777619u;
        }

        return hash;
    }

    [[nodiscard]]
    inline std::uint32_t HashRuntime(std::string_view text) noexcept
    {
        std::uint32_t hash = 2166136261u;

        for (const char character : text)
        {
            hash ^= static_cast<std::uint8_t>(character);
            hash *= 16777619u;
        }

        return hash;
    }

    struct Vec2
    {
        float x = 0.0f;
        float y = 0.0f;

        [[nodiscard]] float LengthSquared() const noexcept
        {
            return x * x + y * y;
        }

        [[nodiscard]] float Length() const noexcept
        {
            return std::sqrt(LengthSquared());
        }

        Vec2& operator+=(const Vec2& rhs) noexcept
        {
            x += rhs.x;
            y += rhs.y;
            return *this;
        }

        Vec2& operator*=(float scalar) noexcept
        {
            x *= scalar;
            y *= scalar;
            return *this;
        }
    };

    [[nodiscard]] inline Vec2 operator+(Vec2 lhs, const Vec2& rhs) noexcept
    {
        lhs += rhs;
        return lhs;
    }

    [[nodiscard]] inline Vec2 operator*(Vec2 value, float scalar) noexcept
    {
        value *= scalar;
        return value;
    }

    [[nodiscard]]
    inline float ClampUnit(float value) noexcept
    {
        return std::clamp(value, -1.0f, 1.0f);
    }

    [[nodiscard]]
    inline float ApplyDeadZone(float value, float deadZone) noexcept
    {
        deadZone = std::clamp(deadZone, 0.0f, 0.9999f);

        const float magnitude = std::abs(value);

        if (magnitude <= deadZone)
        {
            return 0.0f;
        }

        const float remapped =
            (magnitude - deadZone) / (1.0f - deadZone);

        return std::copysign(
            std::clamp(remapped, 0.0f, 1.0f),
            value);
    }

    [[nodiscard]]
    inline Vec2 ApplyRadialDeadZone(
        Vec2 value,
        float deadZone,
        bool normalizeResult = false) noexcept
    {
        deadZone = std::clamp(deadZone, 0.0f, 0.9999f);

        const float magnitude = value.Length();

        if (magnitude <= deadZone ||
            magnitude <= std::numeric_limits<float>::epsilon())
        {
            return {};
        }

        const float clampedMagnitude = std::min(magnitude, 1.0f);
        float newMagnitude =
            (clampedMagnitude - deadZone) / (1.0f - deadZone);

        newMagnitude = std::clamp(newMagnitude, 0.0f, 1.0f);

        if (normalizeResult)
        {
            newMagnitude = 1.0f;
        }

        const float scale = newMagnitude / magnitude;

        return { value.x * scale, value.y * scale };
    }

    enum class DeviceType : std::uint8_t
    {
        Keyboard,
        Mouse,
        Gamepad
    };

    enum class ActionType : std::uint8_t
    {
        Button,
        Axis1D,
        Axis2D
    };

    enum class ActionPhase : std::uint8_t
    {
        Disabled,
        Waiting,
        Started,
        Performed,
        Canceled
    };

    enum class ControlType : std::uint8_t
    {
        KeyboardKey,
        MouseButton,
        MouseDeltaX,
        MouseDeltaY,
        MouseWheelX,
        MouseWheelY,
        GamepadButton,
        GamepadAxis
    };

    enum class GamepadAxis : std::uint8_t
    {
        LeftX,
        LeftY,
        RightX,
        RightY,
        LeftTrigger,
        RightTrigger
    };

    struct Control
    {
        DeviceType device = DeviceType::Keyboard;
        ControlType type = ControlType::KeyboardKey;
        std::uint32_t code = 0;

        [[nodiscard]]
        static constexpr Control Key(
            std::uint32_t virtualKey) noexcept
        {
            return {
                DeviceType::Keyboard,
                ControlType::KeyboardKey,
                virtualKey
            };
        }

        [[nodiscard]]
        static constexpr Control MouseButton(
            GameInputMouseButtons button) noexcept
        {
            return {
                DeviceType::Mouse,
                ControlType::MouseButton,
                static_cast<std::uint32_t>(button)
            };
        }

        [[nodiscard]]
        static constexpr Control MouseDeltaX() noexcept
        {
            return {
                DeviceType::Mouse,
                ControlType::MouseDeltaX,
                0
            };
        }

        [[nodiscard]]
        static constexpr Control MouseDeltaY() noexcept
        {
            return {
                DeviceType::Mouse,
                ControlType::MouseDeltaY,
                0
            };
        }

        [[nodiscard]]
        static constexpr Control MouseWheelX() noexcept
        {
            return {
                DeviceType::Mouse,
                ControlType::MouseWheelX,
                0
            };
        }

        [[nodiscard]]
        static constexpr Control MouseWheelY() noexcept
        {
            return {
                DeviceType::Mouse,
                ControlType::MouseWheelY,
                0
            };
        }

        [[nodiscard]]
        static constexpr Control GamepadButton(
            GameInputGamepadButtons button) noexcept
        {
            return {
                DeviceType::Gamepad,
                ControlType::GamepadButton,
                static_cast<std::uint32_t>(button)
            };
        }

        [[nodiscard]]
        static constexpr Control GamepadAxis(
            ::game::input::GamepadAxis axis) noexcept
        {
            return {
                DeviceType::Gamepad,
                ControlType::GamepadAxis,
                static_cast<std::uint32_t>(axis)
            };
        }
    };

    struct ButtonBinding
    {
        Control control{};
        float pressPoint = 0.5f;
    };

    struct Axis1DBinding
    {
        Control negative{};
        Control positive{};
    };

    struct Composite2DBinding
    {
        Control up{};
        Control down{};
        Control left{};
        Control right{};
        bool normalize = true;
    };

    struct Stick2DBinding
    {
        Control xAxis{};
        Control yAxis{};
        float deadZone = 0.15f;
        bool invertY = false;
    };

    using BindingData = std::variant<
        ButtonBinding,
        Axis1DBinding,
        Composite2DBinding,
        Stick2DBinding>;

    struct Binding
    {
        BindingData data{};
        float scale = 1.0f;
        float deadZone = 0.0f;

        [[nodiscard]]
        static Binding Button(
            Control control,
            float pressPoint = 0.5f,
            float scale = 1.0f) noexcept
        {
            return {
                ButtonBinding{control, pressPoint},
                scale,
                0.0f
            };
        }

        [[nodiscard]]
        static Binding Axis1D(
            Control negative,
            Control positive,
            float scale = 1.0f,
            float deadZone = 0.0f) noexcept
        {
            return {
                Axis1DBinding{negative, positive},
                scale,
                deadZone
            };
        }

        [[nodiscard]]
        static Binding Composite2D(
            Control up,
            Control down,
            Control left,
            Control right,
            bool normalize = true,
            float scale = 1.0f) noexcept
        {
            return {
                Composite2DBinding{
                    up, down, left, right, normalize},
                scale,
                0.0f
            };
        }

        [[nodiscard]]
        static Binding Stick2D(
            Control xAxis,
            Control yAxis,
            float deadZone = 0.15f,
            bool invertY = false,
            float scale = 1.0f) noexcept
        {
            return {
                Stick2DBinding{
                    xAxis, yAxis, deadZone, invertY},
                scale,
                deadZone
            };
        }
    };

    struct ActionState
    {
        ActionPhase phase = ActionPhase::Waiting;

        bool currentPressed = false;
        bool previousPressed = false;

        float current1D = 0.0f;
        float previous1D = 0.0f;

        Vec2 current2D{};
        Vec2 previous2D{};

        float heldSeconds = 0.0f;

        [[nodiscard]]
        bool IsPressed() const noexcept
        {
            return currentPressed;
        }

        [[nodiscard]]
        bool WasPressedThisFrame() const noexcept
        {
            return currentPressed && !previousPressed;
        }

        [[nodiscard]]
        bool WasReleasedThisFrame() const noexcept
        {
            return !currentPressed && previousPressed;
        }
    };

    class InputAction
    {
    public:
        InputAction() = default;

        InputAction(
            ActionId id,
            std::string name,
            ActionType type) :
            m_id(id),
            m_name(std::move(name)),
            m_type(type)
        {
        }

        InputAction& AddBinding(Binding binding)
        {
            m_bindings.push_back(std::move(binding));
            return *this;
        }

        void ClearBindings()
        {
            m_bindings.clear();
        }

        void SetEnabled(bool enabled) noexcept
        {
            m_enabled = enabled;
        }

        [[nodiscard]] bool IsEnabled() const noexcept
        {
            return m_enabled;
        }

        [[nodiscard]] ActionId Id() const noexcept
        {
            return m_id;
        }

        [[nodiscard]] const std::string& Name() const noexcept
        {
            return m_name;
        }

        [[nodiscard]] ActionType Type() const noexcept
        {
            return m_type;
        }

        [[nodiscard]] std::span<const Binding> Bindings() const noexcept
        {
            return m_bindings;
        }

        [[nodiscard]] std::span<Binding> Bindings() noexcept
        {
            return m_bindings;
        }

        [[nodiscard]] const ActionState& State() const noexcept
        {
            return m_state;
        }

    private:
        friend class InputSystem;

        ActionId m_id = 0;
        std::string m_name;
        ActionType m_type = ActionType::Button;
        std::vector<Binding> m_bindings;
        ActionState m_state{};
        bool m_enabled = true;
    };

    class InputActionMap
    {
    public:
        InputActionMap() = default;

        InputActionMap(MapId id, std::string name) :
            m_id(id),
            m_name(std::move(name))
        {
        }

        InputAction& AddAction(InputAction action)
        {
            m_actions.push_back(std::move(action));
            return m_actions.back();
        }

        void ClearActions()
        {
            m_actions.clear();
        }

        [[nodiscard]]
        const InputAction* FindAction(ActionId actionId) const noexcept
        {
            const auto iterator = std::find_if(
                m_actions.begin(),
                m_actions.end(),
                [actionId](const InputAction& action)
                {
                    return action.Id() == actionId;
                });

            return iterator == m_actions.end()
                ? nullptr
                : &(*iterator);
        }

        void SetEnabled(bool enabled) noexcept
        {
            m_enabled = enabled;
        }

        [[nodiscard]] bool IsEnabled() const noexcept
        {
            return m_enabled;
        }

        [[nodiscard]] MapId Id() const noexcept
        {
            return m_id;
        }

        [[nodiscard]] const std::string& Name() const noexcept
        {
            return m_name;
        }

        [[nodiscard]]
        std::span<const InputAction> Actions() const noexcept
        {
            return m_actions;
        }

    private:
        friend class InputSystem;

        [[nodiscard]]
        std::span<InputAction> MutableActions() noexcept
        {
            return m_actions;
        }

        [[nodiscard]]
        InputAction* FindActionMutable(ActionId actionId) noexcept
        {
            const auto iterator = std::find_if(
                m_actions.begin(),
                m_actions.end(),
                [actionId](const InputAction& action)
                {
                    return action.Id() == actionId;
                });

            return iterator == m_actions.end()
                ? nullptr
                : &(*iterator);
        }

        MapId m_id = 0;
        std::string m_name;
        std::vector<InputAction> m_actions;
        bool m_enabled = true;
    };

    class GameInputBackend
    {
    public:
        GameInputBackend() = default;

        GameInputBackend(const GameInputBackend&) = delete;
        GameInputBackend& operator=(const GameInputBackend&) = delete;

        [[nodiscard]]
        bool Initialize() noexcept
        {
            if (m_gameInput)
            {
                return true;
            }

            return SUCCEEDED(GameInputCreate(&m_gameInput));
        }

        void Shutdown() noexcept
        {
            m_gamepadDevice.Reset();
            m_gameInput.Reset();

            m_keys.reset();
            m_mouse = {};
            m_gamepad = {};
            m_mouseDelta = {};
            m_mouseWheel = {};
            m_initializedMouseReading = false;
        }

        [[nodiscard]]
        bool IsInitialized() const noexcept
        {
            return static_cast<bool>(m_gameInput);
        }

        void Update() noexcept
        {
            if (!m_gameInput)
            {
                return;
            }

            PollKeyboard();
            PollMouse();
            PollGamepad();
        }

        [[nodiscard]]
        bool IsKeyDown(std::uint32_t virtualKey) const noexcept
        {
            return virtualKey < m_keys.size()
                ? m_keys.test(virtualKey)
                : false;
        }

        [[nodiscard]]
        bool IsMouseButtonDown(
            GameInputMouseButtons button) const noexcept
        {
            return (m_mouse.buttons & button) != 0;
        }

        [[nodiscard]]
        bool IsGamepadButtonDown(
            GameInputGamepadButtons button) const noexcept
        {
            return (m_gamepad.buttons & button) != 0;
        }

        [[nodiscard]] Vec2 MouseDelta() const noexcept
        {
            return m_mouseDelta;
        }

        [[nodiscard]] Vec2 MouseWheel() const noexcept
        {
            return m_mouseWheel;
        }

        [[nodiscard]]
        float ReadGamepadAxis(GamepadAxis axis) const noexcept
        {
            switch (axis)
            {
            case GamepadAxis::LeftX:
                return m_gamepad.leftThumbstickX;

            case GamepadAxis::LeftY:
                return m_gamepad.leftThumbstickY;

            case GamepadAxis::RightX:
                return m_gamepad.rightThumbstickX;

            case GamepadAxis::RightY:
                return m_gamepad.rightThumbstickY;

            case GamepadAxis::LeftTrigger:
                return m_gamepad.leftTrigger;

            case GamepadAxis::RightTrigger:
                return m_gamepad.rightTrigger;
            }

            return 0.0f;
        }

    private:
        void PollKeyboard() noexcept
        {
            m_keys.reset();

            Microsoft::WRL::ComPtr<IGameInputReading> reading;

            if (FAILED(m_gameInput->GetCurrentReading(
                GameInputKindKeyboard,
                nullptr,
                &reading)))
            {
                return;
            }

            const std::uint32_t keyCount = reading->GetKeyCount();

            if (keyCount == 0)
            {
                return;
            }

            m_keyScratch.resize(keyCount);

            const std::uint32_t written = reading->GetKeyState(
                keyCount,
                m_keyScratch.data());

            for (std::uint32_t index = 0;
                index < written;
                ++index)
            {
                const std::uint32_t virtualKey =
                    static_cast<std::uint32_t>(
                        m_keyScratch[index].virtualKey);

                if (virtualKey < m_keys.size())
                {
                    m_keys.set(virtualKey);
                }
            }
        }

        void PollMouse() noexcept
        {
            m_mouseDelta = {};
            m_mouseWheel = {};

            Microsoft::WRL::ComPtr<IGameInputReading> reading;

            if (FAILED(m_gameInput->GetCurrentReading(
                GameInputKindMouse,
                nullptr,
                &reading)))
            {
                m_mouse = {};
                m_initializedMouseReading = false;
                return;
            }

            GameInputMouseState state{};

            if (!reading->GetMouseState(&state))
            {
                m_mouse = {};
                m_initializedMouseReading = false;
                return;
            }

            if (m_initializedMouseReading)
            {
                // positionX/Y and wheelX/Y are cumulative values.
                m_mouseDelta.x = static_cast<float>(
                    state.positionX - m_mouse.positionX);
                m_mouseDelta.y = static_cast<float>(
                    state.positionY - m_mouse.positionY);

                m_mouseWheel.x = static_cast<float>(
                    state.wheelX - m_mouse.wheelX);
                m_mouseWheel.y = static_cast<float>(
                    state.wheelY - m_mouse.wheelY);
            }

            m_mouse = state;
            m_initializedMouseReading = true;
        }

        void PollGamepad() noexcept
        {
            Microsoft::WRL::ComPtr<IGameInputReading> reading;

            const HRESULT result = m_gameInput->GetCurrentReading(
                GameInputKindGamepad,
                m_gamepadDevice.Get(),
                &reading);

            if (FAILED(result))
            {
                // Retry against any gamepad. This also recovers from
                // a disconnected device.
                m_gamepadDevice.Reset();

                if (FAILED(m_gameInput->GetCurrentReading(
                    GameInputKindGamepad,
                    nullptr,
                    &reading)))
                {
                    m_gamepad = {};
                    return;
                }
            }

            GameInputGamepadState state{};

            if (!reading->GetGamepadState(&state))
            {
                m_gamepad = {};
                return;
            }

            if (!m_gamepadDevice)
            {
                Microsoft::WRL::ComPtr<IGameInputDevice> device;
                reading->GetDevice(&device);
                m_gamepadDevice = std::move(device);
            }

            m_gamepad = state;
        }

        Microsoft::WRL::ComPtr<IGameInput> m_gameInput;
        Microsoft::WRL::ComPtr<IGameInputDevice> m_gamepadDevice;

        std::bitset<256> m_keys;
        std::vector<GameInputKeyState> m_keyScratch;

        GameInputMouseState m_mouse{};
        GameInputGamepadState m_gamepad{};

        Vec2 m_mouseDelta{};
        Vec2 m_mouseWheel{};

        bool m_initializedMouseReading = false;
    };

    class InputSystem
    {
    public:
        InputSystem() = default;

        InputSystem(const InputSystem&) = delete;
        InputSystem& operator=(const InputSystem&) = delete;

        [[nodiscard]]
        bool Initialize() noexcept
        {
            return m_backend.Initialize();
        }

        void Shutdown() noexcept
        {
            m_actionLookup.clear();
            m_maps.clear();
            m_backend.Shutdown();
        }

        [[nodiscard]]
        bool IsInitialized() const noexcept
        {
            return m_backend.IsInitialized();
        }

        bool AddMap(InputActionMap map)
        {
            if (map.Id() == 0 || m_maps.contains(map.Id()))
            {
                return false;
            }

            for (const InputAction& action : map.Actions())
            {
                if (action.Id() == 0 ||
                    m_actionLookup.contains(action.Id()))
                {
                    return false;
                }
            }

            const MapId mapId = map.Id();
            auto [iterator, inserted] =
                m_maps.emplace(mapId, std::move(map));

            if (!inserted)
            {
                return false;
            }

            RebuildLookup();
            return true;
        }

        bool RemoveMap(MapId mapId)
        {
            const std::size_t removed = m_maps.erase(mapId);

            if (removed != 0)
            {
                RebuildLookup();
                return true;
            }

            return false;
        }

        bool AddAction(MapId mapId, InputAction action)
        {
            if (action.Id() == 0 ||
                m_actionLookup.contains(action.Id()))
            {
                return false;
            }

            InputActionMap* map = FindMapMutable(mapId);
            if (!map)
            {
                return false;
            }

            map->m_actions.push_back(std::move(action));
            RebuildLookup();
            return true;
        }

        bool RemoveAction(ActionId actionId)
        {
            const auto lookupIterator = m_actionLookup.find(actionId);
            if (lookupIterator == m_actionLookup.end())
            {
                return false;
            }

            InputActionMap* map = FindMapMutable(lookupIterator->second.mapId);
            if (!map)
            {
                RebuildLookup();
                return false;
            }

            auto& actions = map->m_actions;
            const auto actionIterator = std::find_if(
                actions.begin(),
                actions.end(),
                [actionId](const InputAction& action)
                {
                    return action.Id() == actionId;
                });

            if (actionIterator == actions.end())
            {
                RebuildLookup();
                return false;
            }

            actions.erase(actionIterator);
            RebuildLookup();
            return true;
        }

        [[nodiscard]]
        const InputActionMap* FindMap(MapId mapId) const noexcept
        {
            const auto iterator = m_maps.find(mapId);

            return iterator == m_maps.end()
                ? nullptr
                : &iterator->second;
        }

        [[nodiscard]]
        InputAction* FindAction(ActionId actionId) noexcept
        {
            const auto iterator = m_actionLookup.find(actionId);

            return iterator == m_actionLookup.end()
                ? nullptr
                : ResolveAction(iterator->second);
        }

        [[nodiscard]]
        const InputAction* FindAction(ActionId actionId) const noexcept
        {
            const auto iterator = m_actionLookup.find(actionId);

            return iterator == m_actionLookup.end()
                ? nullptr
                : ResolveAction(iterator->second);
        }

        void EnableMap(MapId mapId) noexcept
        {
            if (InputActionMap* map = FindMapMutable(mapId))
            {
                map->SetEnabled(true);
            }
        }

        void DisableMap(MapId mapId) noexcept
        {
            if (InputActionMap* map = FindMapMutable(mapId))
            {
                map->SetEnabled(false);
            }
        }

        void EnableAction(ActionId actionId) noexcept
        {
            if (InputAction* action = FindAction(actionId))
            {
                action->SetEnabled(true);
            }
        }

        void DisableAction(ActionId actionId) noexcept
        {
            if (InputAction* action = FindAction(actionId))
            {
                action->SetEnabled(false);
            }
        }

        void Update(float deltaTime) noexcept
        {
            if (!m_backend.IsInitialized())
            {
                return;
            }

            m_backend.Update();

            for (auto& [mapId, map] : m_maps)
            {
                (void)mapId;

                for (InputAction& action : map.m_actions)
                {
                    UpdateAction(action, map.IsEnabled(), deltaTime);
                }
            }
        }

        [[nodiscard]]
        bool IsPressed(ActionId actionId) const noexcept
        {
            const InputAction* action = FindAction(actionId);
            return action ? action->State().IsPressed() : false;
        }

        [[nodiscard]]
        bool WasPressedThisFrame(ActionId actionId) const noexcept
        {
            const InputAction* action = FindAction(actionId);
            return action
                ? action->State().WasPressedThisFrame()
                : false;
        }

        [[nodiscard]]
        bool WasReleasedThisFrame(ActionId actionId) const noexcept
        {
            const InputAction* action = FindAction(actionId);
            return action
                ? action->State().WasReleasedThisFrame()
                : false;
        }

        [[nodiscard]]
        ActionPhase Phase(ActionId actionId) const noexcept
        {
            const InputAction* action = FindAction(actionId);
            return action
                ? action->State().phase
                : ActionPhase::Disabled;
        }

        [[nodiscard]]
        float HeldSeconds(ActionId actionId) const noexcept
        {
            const InputAction* action = FindAction(actionId);
            return action ? action->State().heldSeconds : 0.0f;
        }

        [[nodiscard]]
        float ReadFloat(ActionId actionId) const noexcept
        {
            const InputAction* action = FindAction(actionId);
            return action ? action->State().current1D : 0.0f;
        }

        [[nodiscard]]
        Vec2 ReadVector2(ActionId actionId) const noexcept
        {
            const InputAction* action = FindAction(actionId);
            return action ? action->State().current2D : Vec2{};
        }

        bool Rebind(
            ActionId actionId,
            std::size_t bindingIndex,
            Binding replacement)
        {
            InputAction* action = FindAction(actionId);

            if (!action ||
                bindingIndex >= action->m_bindings.size())
            {
                return false;
            }

            action->m_bindings[bindingIndex] =
                std::move(replacement);

            return true;
        }

        [[nodiscard]]
        const GameInputBackend& Backend() const noexcept
        {
            return m_backend;
        }

#if GAME_INPUT_SYSTEM_HAS_ENTT
        static InputSystem& InstallInRegistry(
            entt::registry& registry)
        {
            if (auto* existing = registry.ctx().template find<InputSystem>())
            {
                return *existing;
            }

            InputSystem& system =
                registry.ctx().template emplace<InputSystem>();

            (void)system.Initialize();
            return system;
        }

        static InputSystem& GetFromRegistry(
            entt::registry& registry)
        {
            return registry.ctx().template get<InputSystem>();
        }

        static const InputSystem& GetFromRegistry(
            const entt::registry& registry)
        {
            return registry.ctx().template get<InputSystem>();
        }
#endif

    private:
        struct ActionHandle
        {
            MapId mapId = 0;
            std::size_t actionIndex = 0;
        };

        [[nodiscard]]
        InputActionMap* FindMapMutable(MapId mapId) noexcept
        {
            const auto iterator = m_maps.find(mapId);

            return iterator == m_maps.end()
                ? nullptr
                : &iterator->second;
        }

        [[nodiscard]]
        InputAction* ResolveAction(const ActionHandle& handle) noexcept
        {
            InputActionMap* map = FindMapMutable(handle.mapId);
            if (!map || handle.actionIndex >= map->m_actions.size())
            {
                return nullptr;
            }

            return &map->m_actions[handle.actionIndex];
        }

        [[nodiscard]]
        const InputAction* ResolveAction(const ActionHandle& handle) const noexcept
        {
            const InputActionMap* map = FindMap(handle.mapId);
            if (!map || handle.actionIndex >= map->m_actions.size())
            {
                return nullptr;
            }

            return &map->m_actions[handle.actionIndex];
        }

        [[nodiscard]]
        float ReadControl(const Control& control) const noexcept
        {
            switch (control.type)
            {
            case ControlType::KeyboardKey:
                return m_backend.IsKeyDown(control.code)
                    ? 1.0f
                    : 0.0f;

            case ControlType::MouseButton:
                return m_backend.IsMouseButtonDown(
                    static_cast<GameInputMouseButtons>(
                        control.code))
                    ? 1.0f
                    : 0.0f;

            case ControlType::MouseDeltaX:
                return m_backend.MouseDelta().x;

            case ControlType::MouseDeltaY:
                return m_backend.MouseDelta().y;

            case ControlType::MouseWheelX:
                return m_backend.MouseWheel().x;

            case ControlType::MouseWheelY:
                return m_backend.MouseWheel().y;

            case ControlType::GamepadButton:
                return m_backend.IsGamepadButtonDown(
                    static_cast<GameInputGamepadButtons>(
                        control.code))
                    ? 1.0f
                    : 0.0f;

            case ControlType::GamepadAxis:
                return m_backend.ReadGamepadAxis(
                    static_cast<GamepadAxis>(control.code));
            }

            return 0.0f;
        }

        [[nodiscard]]
        float EvaluateButton(
            const ButtonBinding& binding) const noexcept
        {
            return ReadControl(binding.control);
        }

        [[nodiscard]]
        float EvaluateAxis1D(
            const Axis1DBinding& binding,
            float deadZone) const noexcept
        {
            const float negative =
                ReadControl(binding.negative);
            const float positive =
                ReadControl(binding.positive);

            return ApplyDeadZone(
                ClampUnit(positive - negative),
                deadZone);
        }

        [[nodiscard]]
        Vec2 EvaluateComposite2D(
            const Composite2DBinding& binding) const noexcept
        {
            Vec2 value{
                ReadControl(binding.right) -
                    ReadControl(binding.left),

                ReadControl(binding.up) -
                    ReadControl(binding.down)
            };

            const float length = value.Length();

            if (length > 1.0f ||
                (binding.normalize && length > 0.0f))
            {
                value.x /= length;
                value.y /= length;
            }

            return value;
        }

        [[nodiscard]]
        Vec2 EvaluateStick2D(
            const Stick2DBinding& binding) const noexcept
        {
            Vec2 value{
                ReadControl(binding.xAxis),
                ReadControl(binding.yAxis)
            };

            if (binding.invertY)
            {
                value.y = -value.y;
            }

            return ApplyRadialDeadZone(
                value,
                binding.deadZone,
                false);
        }

        void BeginActionFrame(ActionState& state) noexcept
        {
            state.previousPressed = state.currentPressed;
            state.previous1D = state.current1D;
            state.previous2D = state.current2D;

            state.currentPressed = false;
            state.current1D = 0.0f;
            state.current2D = {};
        }

        void FinishActionFrame(
            ActionState& state,
            bool enabled,
            float deltaTime) noexcept
        {
            if (!enabled)
            {
                state.currentPressed = false;
                state.current1D = 0.0f;
                state.current2D = {};
                state.heldSeconds = 0.0f;
                state.phase = ActionPhase::Disabled;
                return;
            }

            if (state.currentPressed)
            {
                state.heldSeconds =
                    state.previousPressed
                    ? state.heldSeconds + std::max(deltaTime, 0.0f)
                    : 0.0f;
            }
            else
            {
                state.heldSeconds = 0.0f;
            }

            if (state.currentPressed &&
                !state.previousPressed)
            {
                state.phase = ActionPhase::Started;
            }
            else if (state.currentPressed)
            {
                state.phase = ActionPhase::Performed;
            }
            else if (!state.currentPressed &&
                state.previousPressed)
            {
                state.phase = ActionPhase::Canceled;
            }
            else
            {
                state.phase = ActionPhase::Waiting;
            }
        }

        void UpdateAction(
            InputAction& action,
            bool mapEnabled,
            float deltaTime) noexcept
        {
            ActionState& state = action.m_state;
            BeginActionFrame(state);

            const bool enabled =
                mapEnabled && action.IsEnabled();

            if (!enabled)
            {
                FinishActionFrame(
                    state,
                    false,
                    deltaTime);
                return;
            }

            switch (action.Type())
            {
            case ActionType::Button:
            {
                float strongest = 0.0f;
                float pressPoint = 0.5f;

                for (const Binding& binding :
                    action.m_bindings)
                {
                    const auto* button =
                        std::get_if<ButtonBinding>(
                            &binding.data);

                    if (!button)
                    {
                        continue;
                    }

                    const float value =
                        EvaluateButton(*button) *
                        binding.scale;

                    if (std::abs(value) >
                        std::abs(strongest))
                    {
                        strongest = value;
                        pressPoint = button->pressPoint;
                    }
                }

                state.current1D = strongest;
                state.currentPressed =
                    std::abs(strongest) >= pressPoint;
                break;
            }

            case ActionType::Axis1D:
            {
                float accumulated = 0.0f;

                for (const Binding& binding :
                    action.m_bindings)
                {
                    if (const auto* axis =
                        std::get_if<Axis1DBinding>(
                            &binding.data))
                    {
                        accumulated +=
                            EvaluateAxis1D(
                                *axis,
                                binding.deadZone) *
                            binding.scale;
                    }
                    else if (const auto* button =
                        std::get_if<ButtonBinding>(
                            &binding.data))
                    {
                        accumulated +=
                            EvaluateButton(*button) *
                            binding.scale;
                    }
                }

                state.current1D =
                    ClampUnit(accumulated);

                state.currentPressed =
                    std::abs(state.current1D) >= 0.5f;
                break;
            }

            case ActionType::Axis2D:
            {
                Vec2 accumulated{};

                for (const Binding& binding :
                    action.m_bindings)
                {
                    Vec2 value{};

                    if (const auto* composite =
                        std::get_if<Composite2DBinding>(
                            &binding.data))
                    {
                        value =
                            EvaluateComposite2D(
                                *composite);
                    }
                    else if (const auto* stick =
                        std::get_if<Stick2DBinding>(
                            &binding.data))
                    {
                        value =
                            EvaluateStick2D(*stick);
                    }
                    else
                    {
                        continue;
                    }

                    accumulated += value * binding.scale;
                }

                const float length = accumulated.Length();

                if (length > 1.0f)
                {
                    accumulated.x /= length;
                    accumulated.y /= length;
                }

                state.current2D = accumulated;
                state.currentPressed =
                    accumulated.LengthSquared() >= 0.25f;
                break;
            }
            }

            FinishActionFrame(
                state,
                true,
                deltaTime);
        }

        void RebuildLookup()
        {
            m_actionLookup.clear();

            for (auto& [mapId, map] : m_maps)
            {
                for (std::size_t actionIndex = 0;
                    actionIndex < map.m_actions.size();
                    ++actionIndex)
                {
                    InputAction& action = map.m_actions[actionIndex];
                    m_actionLookup.emplace(
                        action.Id(),
                        ActionHandle{ mapId, actionIndex });
                }
            }
        }

        GameInputBackend m_backend;
        std::unordered_map<MapId, InputActionMap> m_maps;
        std::unordered_map<ActionId, ActionHandle> m_actionLookup;
    };

    /*
        Optional ECS component for local multiplayer or entities that need
        to identify the action map controlling them.

        Current backend version tracks one active gamepad. For full local
        multiplayer, this component can be paired later with per-device state.
    */
    struct PlayerInputComponent
    {
        std::uint32_t userIndex = 0;
        MapId actionMap = 0;
    };
}

#undef GAME_INPUT_SYSTEM_HAS_ENTT
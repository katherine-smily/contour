#include <terminal/ControlCode.h>
#include <terminal/Parser.h>
#include <terminal/ParserTables.h>
#include <terminal/UTF8.h>
#include <terminal/Util.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <iostream>

#include <fmt/format.h>

namespace terminal {

using namespace std;

using Range = ParserTable::Range;

constexpr bool includes(Range const& range, wchar_t value) noexcept
{
    return range.first <= value && value <= range.last;
}

constexpr bool isExecuteChar(wchar_t value) noexcept
{
    return includes(Range{0x00, 0x17}, value) || value == 0x19 || includes(Range{0x1C, 0x1F}, value);
}

constexpr bool isParamChar(wchar_t value) noexcept
{
    return includes(Range{0x30, 0x39}, value) || value == 0x3B;
}

constexpr bool isC1(wchar_t value) noexcept
{
    return includes(Range{0x80, 0x9F}, value);
}

constexpr bool isPrintChar(wchar_t value) noexcept
{
    return includes(Range{0x20, 0x7F}, value) || (value > 0x7F && !isC1(value));
}

void Parser::parseFragment(uint8_t const* begin, uint8_t const* end)
{
    // log("initial state: {}, processing {} bytes: {}", to_string(state_), distance(begin, end),
    //     escape(begin, end));

    begin_ = begin;
    end_ = end;

    parse();
}

void Parser::parse()
{
    while (dataAvailable())
    {
        currentChar_ = 0;
        visit(
            overloaded{
                [&](utf8::Decoder::Incomplete) {},
                [&](utf8::Decoder::Invalid invalid) {
                    log("Invalid UTF8!");
                    currentChar_ = invalid.replacementCharacter;
                    handleViaTables();
                },
                [&](utf8::Decoder::Success success) {
                    currentChar_ = success.value;
                    handleViaTables();
                },
            },
            utf8Decoder_.decode(currentByte()));

        advance();
    }
}

void Parser::logInvalidInput() const
{
    if (isprint(currentChar()))
        log("{}: invalid character: {:02X} '{}'", to_string(state_), static_cast<unsigned>(currentChar()),
            static_cast<char>(currentChar()));
    else
        log("{}: invalid character: {:02X}", to_string(state_), static_cast<unsigned>(currentChar()));
}

void Parser::logTrace(std::string const& message) const
{
    if (traceLog_)
    {
        string const character =
            currentChar() ? fmt::format("character: {:02X}", static_cast<unsigned>(currentChar())) : string("");

        traceLog_(fmt::format("{}: {}: {:02X} {} {}", to_string(state_), message,
                              static_cast<unsigned>(currentByte()), escape(static_cast<unsigned>(currentByte())),
                              character));
    }
}

void Parser::handleViaTables()
{
    auto const s = static_cast<size_t>(state_);

    ParserTable static constexpr table = ParserTable::get();

    if (state_ == State::Ground && isPrintChar(currentChar()))
        // FIXME a hack I am not yet feeling right with: Eliminate this if-condition.
        invokeAction(ActionClass::Event, Action::Print);
    else if (auto const t = table.transitions[s][currentChar()]; t != State::Undefined)
    {
        invokeAction(ActionClass::Leave, table.exitEvents[s]);
        invokeAction(ActionClass::Transition, table.events[s][currentChar()]);
        state_ = t;
        invokeAction(ActionClass::Enter, table.entryEvents[static_cast<size_t>(t)]);
    }
    else if (Action const a = table.events[s][currentChar()]; a != Action::Undefined)
        invokeAction(ActionClass::Event, a);
    else
        log("Parser Error: Unknown action for state/input pair ({}, {})", to_string(state_),
            escape(currentChar()));
}

void Parser::handleViaSwitch()
{
    logTrace("handle character");

    if (currentChar() == 0x18 || currentChar() == 0x1A || currentChar() == 0x9C || includes(Range{0x80, 0x8F}, currentChar())
        || includes(Range{0x91, 0x97}, currentChar()))
        return transitionTo(State::Ground);
    else if (currentChar() == 0x1B)
        return transitionTo(State::Escape);
    else if (currentChar() == 0x90)
        return transitionTo(State::DCS_Entry);
    else if (currentChar() == 0x98 || currentChar() == 0x9E || currentChar() == 0x9F)
        return transitionTo(State::SOS_PM_APC_String);

    switch (state_)
    {
        case State::Undefined:
            break;

        case State::Ground:
            if (isExecuteChar(currentChar()))
                invokeAction(ActionClass::Event, Action::Execute);
            else if (isPrintChar(currentChar()))
                invokeAction(ActionClass::Event, Action::Print);
            else
                logInvalidInput();
            break;

        case State::Escape:
            if (isExecuteChar(currentChar()))
                invokeAction(ActionClass::Event, Action::Execute);
            else if (currentChar() == 0x7F)
                invokeAction(ActionClass::Event, Action::Ignore);
            else if (currentChar() == 0x58 || currentChar() == 0x5E || currentChar() == 0x5F)
                transitionTo(State::SOS_PM_APC_String);
            else if (currentChar() == 0x50)
                transitionTo(State::DCS_Entry);
            else if (currentChar() == 0x5D)
                transitionTo(State::OSC_String);
            else if (currentChar() == 0x5B)
                transitionTo(State::CSI_Entry);
            else if (includes(Range{0x30, 0x4F}, currentChar()) || includes(Range{0x51, 0x57}, currentChar()) || currentChar() == 0x59
                     || currentChar() == 0x5A || currentChar() == 0x5C || includes(Range{0x60, 0x7E}, currentChar()))
                transitionTo(State::Ground, Action::ESC_Dispatch);
            else if (includes(Range{0x20, 0x2F}, currentChar()))
                transitionTo(State::EscapeIntermediate, Action::Collect);
            else
                logInvalidInput();
            break;

        case State::EscapeIntermediate:
            if (isExecuteChar(currentChar()))
                invokeAction(ActionClass::Event, Action::Execute);
            else if (includes(Range{0x20, 0x2F}, currentChar()))
                invokeAction(ActionClass::Event, Action::Collect);
            else if (currentChar() == 0x7F)
                invokeAction(ActionClass::Event, Action::Ignore);
            else if (includes(Range{0x30, 0x7E}, currentChar()))
                transitionTo(State::Ground, Action::ESC_Dispatch);
            else
                logInvalidInput();
            break;

        case State::CSI_Entry:
            if (isExecuteChar(currentChar()))
                invokeAction(ActionClass::Event, Action::Execute);
            else if (currentChar() == 0x7F)
                invokeAction(ActionClass::Event, Action::Ignore);
            else if (includes(Range{0x40, 0x7E}, currentChar()))
                transitionTo(State::Ground, Action::CSI_Dispatch);
            else if (includes(Range{0x20, 0x2F}, currentChar()))
                transitionTo(State::CSI_Intermediate, Action::Collect);
            else if (currentChar() == 0x3A)
                invokeAction(ActionClass::Event, Action::Ignore);
            else if (isParamChar(currentChar()))
                invokeAction(ActionClass::Event, Action::Param);
            else if (includes(Range{0x3C, 0x3F}, currentChar()))
                invokeAction(ActionClass::Event, Action::Collect);
            else
                logInvalidInput();
            break;

        case State::CSI_Param:
            if (isExecuteChar(currentChar()))
                invokeAction(ActionClass::Event, Action::Execute);
            else if (isParamChar(currentChar()))
                invokeAction(ActionClass::Event, Action::Param);
            else if (currentChar() == 0x7F)
                invokeAction(ActionClass::Event, Action::Ignore);
            else if (currentChar() == 0x3A || includes(Range{0x3C, 0x3F}, currentChar()))
                transitionTo(State::DCS_Ignore);
            else if (includes(Range{0x20, 0x2F}, currentChar()))
                transitionTo(State::DCS_Intermediate);
            else if (includes(Range{0x40, 0x7E}, currentChar()))
                transitionTo(State::DCS_PassThrough);
            else
                logInvalidInput();
            break;

        case State::CSI_Intermediate:
            if (isExecuteChar(currentChar()))
                invokeAction(ActionClass::Event, Action::Execute);
            else if (includes(Range{0x20, 0x2F}, currentChar()))
                invokeAction(ActionClass::Event, Action::Collect);
            else if (currentChar() == 0x7F)
                invokeAction(ActionClass::Event, Action::Ignore);
            else if (includes(Range{0x30, 0x3F}, currentChar()))
                transitionTo(State::CSI_Ignore);
            else if (includes(Range{0x40, 0x7E}, currentChar()))
                transitionTo(State::Ground, Action::CSI_Dispatch);
            else
                logInvalidInput();
            break;

        case State::CSI_Ignore:
            if (isExecuteChar(currentChar()))
                invokeAction(ActionClass::Event, Action::Execute);
            else if (includes(Range{0x20, 0x3F}, currentChar()) || currentChar() == 0x7F)
                invokeAction(ActionClass::Event, Action::Ignore);
            else if (includes(Range{0x40, 0x7E}, currentChar()))
                transitionTo(State::Ground);
            else
                logInvalidInput();
            break;

        case State::DCS_Entry:
            if (isExecuteChar(currentChar()))
                invokeAction(ActionClass::Event, Action::Execute);
            else if (currentChar() == 0x7F)
                invokeAction(ActionClass::Event, Action::Ignore);
            else if (includes(Range{0x20, 0x2F}, currentChar()))
                transitionTo(State::DCS_Intermediate, Action::Collect);
            else if (currentChar() == 0x3A)
                transitionTo(State::DCS_Ignore);
            else if (isParamChar(currentChar()))
                transitionTo(State::DCS_Param, Action::Param);
            else if (includes(Range{0x3C, 0x3F}, currentChar()))
                transitionTo(State::DCS_Param, Action::Collect);
            else if (includes(Range{0x40, 0x7E}, currentChar()))
                transitionTo(State::DCS_PassThrough);
            else
                logInvalidInput();
            break;

        case State::DCS_Param:
            if (isExecuteChar(currentChar()))
                invokeAction(ActionClass::Event, Action::Execute);
            else if (isParamChar(currentChar()))
                invokeAction(ActionClass::Event, Action::Param);
            else if (currentChar() == 0x7F)
                invokeAction(ActionClass::Event, Action::Ignore);
            else if (currentChar() == 0x3A || includes(Range{0x3C, 0x3F}, currentChar()))
                transitionTo(State::DCS_Ignore);
            else if (includes(Range{0x20, 0x2F}, currentChar()))
                transitionTo(State::DCS_Intermediate);
            else if (includes(Range{0x40, 0x7E}, currentChar()))
                transitionTo(State::DCS_PassThrough);
            else
                logInvalidInput();
            break;

        case State::DCS_Intermediate:
            if (isExecuteChar(currentChar()))
                invokeAction(ActionClass::Event, Action::Execute);
            else if (includes(Range{0x20, 0x2F}, currentChar()))
                invokeAction(ActionClass::Event, Action::Collect);
            else if (currentChar() == 0x7F)
                invokeAction(ActionClass::Event, Action::Ignore);
            else if (includes(Range{0x40, 0x7E}, currentChar()))
                transitionTo(State::DCS_PassThrough);
            else
                logInvalidInput();
            break;

        case State::DCS_PassThrough:
            if (isExecuteChar(currentChar()) || includes(Range{0x20, 0x7E}, currentChar()))
                invokeAction(ActionClass::Event, Action::Put);
            else if (currentChar() == 0x7F)
                invokeAction(ActionClass::Event, Action::Ignore);
            else if (currentChar() == 0x9C)
            {
                invokeAction(ActionClass::Event, Action::Unhook);
                transitionTo(State::Ground);
            }
            else
                logInvalidInput();
            break;

        case State::DCS_Ignore:
            if (isExecuteChar(currentChar()) || includes(Range{0x20, 0x7F}, currentChar()))
                invokeAction(ActionClass::Event, Action::Ignore);
            else if (currentChar() == 0x9C)
                transitionTo(State::Ground);
            else
                logInvalidInput();
            break;

        case State::OSC_String:
            if (isExecuteChar(currentChar()))
                invokeAction(ActionClass::Event, Action::Ignore);
            else if (includes(Range{0x20, 0x7F}, currentChar()))
                invokeAction(ActionClass::Event, Action::OSC_Put);
            else if (currentChar() == 0x9C)
            {
                invokeAction(ActionClass::Leave, Action::OSC_End);
                transitionTo(State::Ground);
            }
            else
                logInvalidInput();
            break;

        case State::SOS_PM_APC_String:
            if (isExecuteChar(currentChar()))
                invokeAction(ActionClass::Event, Action::Ignore);
            else if (currentChar() == 0x9C)
                transitionTo(State::Ground);
            else
                logInvalidInput();
            break;
    }
}

void Parser::invokeAction(ActionClass actionClass, Action action)
{
    // if (action != Action::Ignore && action != Action::Undefined)
    //     log("0x{:02X} '{}' {} {}: {}", currentChar(), escape(currentChar()), to_string(actionClass),
    //         to_string(state_), to_string(action));

    if (actionHandler_)
        actionHandler_(actionClass, action, currentChar());
}

void Parser::transitionTo(State targetState, Action action)
{
    invokeAction(ActionClass::Transition, action);
    state_ = targetState;

    // entry-actions
    switch (targetState)
    {
        case State::Escape:
        case State::CSI_Entry:
        case State::DCS_Entry:
            invokeAction(ActionClass::Enter, Action::Clear);
            break;
        case State::DCS_PassThrough:
            invokeAction(ActionClass::Enter, Action::Hook);
            break;
        case State::OSC_String:
            invokeAction(ActionClass::Enter, Action::OSC_Start);
            break;
        default:
            break;
    }
}

}  // namespace terminal
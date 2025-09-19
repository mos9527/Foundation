#pragma once
namespace Foundation::Native {
    enum class MessageBoxType : int {
        Ok = 0,
        OkCancel = 1,
        YesNo = 2,
        YesNoCancel = 3
    };
    enum class MessageBoxIcon : int {
        Info = 0,
        Warning = 1,
        Error = 2,
        Question = 3
    };
    enum class MessageBoxResult : int {
        No = 0,
        Yes = 1,
        Cancel = 2
    };
}

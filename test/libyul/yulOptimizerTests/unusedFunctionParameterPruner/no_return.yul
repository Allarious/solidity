{
    f(1, 2)
    function f(a, b)
    {
        sstore(a, 0)
        a := add(a, 1)
    }
}
// ----
// step: unusedFunctionParameterPruner
//
// {
//     f_1(1, 2)
//     function f(a)
//     {
//         sstore(a, 0)
//         a := add(a, 1)
//     }
//     function f_1(a_1, b)
//     { f(a_1) }
// }

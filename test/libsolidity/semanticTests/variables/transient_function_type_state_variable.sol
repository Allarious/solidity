contract C {
    function () external transient f;
    function () external f1;
    function g() external {
    }

    function testTransient() public returns (bool) {
        assert(f != this.g);
        f = this.g;

        return f == this.g;
    }

    function testStorage() public returns (bool) {
        assert(f1 != this.g);
        f1 = this.g;

        return f1 == this.g;
    }
}
// ====
// EVMVersion: >=cancun
// compileViaYul: false
// ----
// testTransient() -> false
// testStorage() -> true

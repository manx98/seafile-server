namespace Seafile {

public class FileLock : Object {
    public string repo_id { set; get; }
    public string path { set; get; }
    public string user { set; get; }
    public int64 lock_time { set; get; }
    public int64 expire { set; get; }
}

}

import {useCallback,useRef,useState} from "react";
interface Draft {text:string;dirty:boolean;runtimeChanged:boolean;generation:number;baseline:string}
export function useLiveConfigDraft(){
 const current=useRef<Draft>({text:"",dirty:false,runtimeChanged:false,generation:0,baseline:""});
 const [state,setState]=useState(current.current);
 const update=useCallback((value:Draft)=>{current.current=value;setState(value);},[]);
 const edit=useCallback((text:string)=>{const d=current.current;update({...d,text,dirty:true,generation:d.generation+1});},[update]);
 const generation=useCallback(()=>current.current.generation,[]);
 const acceptRemote=useCallback((text:string,discard=false,requestedGeneration=current.current.generation)=>{
  const d=current.current;
  if((d.dirty&&!discard)||d.generation!==requestedGeneration){update({...d,runtimeChanged:d.runtimeChanged||text!==d.baseline});return false;}
  update({...d,text,baseline:text,dirty:false,runtimeChanged:false});return true;
 },[update]);
 const applied=useCallback((submitted:string)=>{const d=current.current;if(d.text===submitted)update({...d,dirty:false,runtimeChanged:false});},[update]);
 return {...state,edit,generation,acceptRemote,applied};
}
